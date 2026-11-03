#!/usr/bin/env python3
"""Aggregate revealed RifeLove blind-review exports without ranking candidates."""
from __future__ import annotations

import argparse
from collections import defaultdict
from fractions import Fraction
import hashlib
import json
import math
import os
from pathlib import Path
import re
import statistics
import sys
import tempfile
import time


MAX_INPUT_BYTES = 16 * 1024 * 1024
MAX_REVIEW_SESSIONS = 256
MAX_RATINGS_PER_SESSION = 10_000
MAX_TOTAL_RATINGS = 100_000
MAX_TOTAL_NOTE_BYTES = 4 * 1024 * 1024
MAX_NOTE_BYTES = 4_096
MAX_CANDIDATES = 64
MAX_REVIEW_POINT_INDEX = 10_000
SHA256_PATTERN = re.compile(r"[0-9a-f]{64}\Z")
SESSION_ID_PATTERN = re.compile(r"[0-9a-f]{32}\Z")
REVIEWER_CODE_PATTERN = re.compile(r"[A-Za-z0-9][A-Za-z0-9_-]{0,31}\Z")
DRIVE_PATH_PATTERN = re.compile(r"[A-Za-z]:")
ARTIFACT_CODES = (
    "none_obvious",
    "motion_tearing_or_misalignment",
    "ghosting_or_smearing",
    "flicker_or_temporal_instability",
    "texture_or_detail_distortion",
    "occlusion_boundary_error",
    "other_obvious_artifact",
)


def _unique_object(pairs: list[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def _reject_constant(value: str) -> None:
    raise ValueError(f"non-finite JSON number is not allowed: {value}")


def _read_json(path: Path) -> dict[str, object]:
    if not path.is_file():
        raise ValueError(f"review export is not a regular file: {path}")
    size = path.stat().st_size
    if size <= 0 or size > MAX_INPUT_BYTES:
        raise ValueError(f"review export is empty or exceeds {MAX_INPUT_BYTES} bytes: {path}")
    raw = path.read_bytes()
    if len(raw) != size:
        raise ValueError(f"review export changed while being read: {path}")
    try:
        value = json.loads(raw.decode("utf-8-sig"), object_pairs_hook=_unique_object, parse_constant=_reject_constant)
    except (UnicodeError, json.JSONDecodeError, RecursionError) as error:
        raise ValueError(f"invalid review JSON in {path}: {error}") from error
    if not isinstance(value, dict):
        raise ValueError(f"review export root must be a JSON object: {path}")
    return value


def _require_text(value: object, field: str, limit: int = 512) -> str:
    if not isinstance(value, str) or not value:
        raise ValueError(f"{field} must be non-empty UTF-8 text of at most {limit} bytes")
    try:
        encoded_length = len(value.encode("utf-8"))
    except UnicodeEncodeError as error:
        raise ValueError(f"{field} must contain valid Unicode text") from error
    if encoded_length > limit:
        raise ValueError(f"{field} must be non-empty UTF-8 text of at most {limit} bytes")
    if any(ord(character) < 32 or ord(character) == 127 for character in value):
        raise ValueError(f"{field} contains a control character")
    return value


def _require_note(value: object) -> str:
    if not isinstance(value, str) or not value.strip():
        raise ValueError("ratings.note must be non-empty text")
    try:
        encoded_length = len(value.encode("utf-8"))
    except UnicodeEncodeError as error:
        raise ValueError("ratings.note must contain valid Unicode text") from error
    if encoded_length > MAX_NOTE_BYTES:
        raise ValueError(f"ratings.note exceeds {MAX_NOTE_BYTES} UTF-8 bytes")
    if any((ord(character) < 32 and character not in "\t\r\n") or ord(character) == 127 for character in value):
        raise ValueError("ratings.note contains a disallowed control character")
    return value


def _require_integer(value: object, field: str, minimum: int, maximum: int) -> int:
    if type(value) is not int or value < minimum or value > maximum:
        raise ValueError(f"{field} must be an integer between {minimum} and {maximum}")
    return value


def _parse_time(value: object, field: str) -> Fraction:
    if not isinstance(value, str) or not value or len(value) > 64:
        raise ValueError(f"{field} must be a rational timestamp string")
    try:
        result = Fraction(value)
    except (ValueError, ZeroDivisionError) as error:
        raise ValueError(f"{field} is not a valid rational timestamp") from error
    if result < 0:
        raise ValueError(f"{field} cannot be negative")
    return result


def _safe_candidate_name(value: object) -> str:
    name = _require_text(value, "candidateMapping.name")
    if "/" in name or "\\" in name or DRIVE_PATH_PATTERN.match(name):
        raise ValueError("candidateMapping.name must not contain a local path")
    return name


def _load_session(path: Path) -> tuple[str, bool, bool, str | None, dict[str, object], list[dict[str, object]]]:
    document = _read_json(path)
    if document.get("schemaVersion") != 1 or document.get("application") != "RifeLove" or document.get("recordType") != "human_visual_review":
        raise ValueError(f"unsupported review export schema: {path}")
    if document.get("blindReview") is not True or document.get("identitiesIncluded") is not True:
        raise ValueError(f"only identity-revealed blind-review exports can be aggregated: {path}")

    session_id = document.get("sessionId")
    if not isinstance(session_id, str) or not SESSION_ID_PATTERN.fullmatch(session_id):
        raise ValueError(f"invalid review session ID: {path}")
    created_at = _require_integer(document.get("createdAtUnixMs"), "createdAtUnixMs", 1, 2**63 - 1)
    reviewer_code = document.get("reviewerCode")
    if "reviewerCode" in document:
        if not isinstance(reviewer_code, str) or not REVIEWER_CODE_PATTERN.fullmatch(reviewer_code):
            raise ValueError(f"reviewerCode must be 1–32 letters, digits, underscores or hyphens: {path}")
        reviewer_code = reviewer_code.upper()

    mapping = document.get("candidateMapping")
    if not isinstance(mapping, list) or not 2 <= len(mapping) <= MAX_CANDIDATES:
        raise ValueError(f"candidateMapping must contain 2–{MAX_CANDIDATES} candidates: {path}")
    by_label: dict[str, dict[str, object]] = {}
    seen_source_indices: set[int] = set()
    seen_hashes: set[str] = set()
    source_order_complete = True
    for item in mapping:
        if not isinstance(item, dict):
            raise ValueError(f"candidateMapping entries must be objects: {path}")
        label = _require_text(item.get("candidateLabel"), "candidateMapping.candidateLabel", 128)
        name = _safe_candidate_name(item.get("name"))
        digest = item.get("outputSha256")
        if not isinstance(digest, str) or not SHA256_PATTERN.fullmatch(digest):
            raise ValueError(f"candidateMapping.outputSha256 must be lowercase SHA-256 hex: {path}")
        if digest in seen_hashes:
            raise ValueError(f"duplicate candidate output SHA-256 in one session: {path}")
        seen_hashes.add(digest)
        if label in by_label:
            raise ValueError(f"duplicate candidate label in one session: {path}")
        source_index = item.get("sourceCandidateIndex")
        if source_index is None:
            source_order_complete = False
        else:
            source_index = _require_integer(source_index, "sourceCandidateIndex", 0, MAX_CANDIDATES - 1)
            if source_index in seen_source_indices:
                raise ValueError(f"duplicate source candidate index in one session: {path}")
            seen_source_indices.add(source_index)
        by_label[label] = {"name": name, "sha256": digest, "sourceCandidateIndex": source_index}
    if source_order_complete and seen_source_indices != set(range(len(mapping))):
        raise ValueError(f"source candidate indices must be contiguous from zero: {path}")

    randomization = document.get("randomization")
    seed_present = False
    if randomization is not None:
        if not isinstance(randomization, dict) or randomization.get("algorithm") != "std::mt19937 + std::shuffle":
            raise ValueError(f"unsupported randomization metadata: {path}")
        seed_words = randomization.get("seedWords")
        if not isinstance(seed_words, list) or len(seed_words) != 4:
            raise ValueError(f"randomization.seedWords must contain four values: {path}")
        for word in seed_words:
            _require_integer(word, "randomization.seedWords[]", 0, 2**32 - 1)
        seed_present = True

    ratings = document.get("ratings")
    if not isinstance(ratings, list) or not 1 <= len(ratings) <= MAX_RATINGS_PER_SESSION:
        raise ValueError(f"ratings must contain 1–{MAX_RATINGS_PER_SESSION} entries: {path}")
    normalized: list[dict[str, object]] = []
    seen_entries: set[tuple[str, int]] = set()
    seen_candidate_points: set[tuple[str, str, Fraction, Fraction | None]] = set()
    point_definitions: dict[int, tuple[str, Fraction, Fraction | None]] = {}
    for entry in ratings:
        if not isinstance(entry, dict):
            raise ValueError(f"ratings entries must be objects: {path}")
        label = _require_text(entry.get("candidateLabel"), "ratings.candidateLabel", 128)
        candidate = by_label.get(label)
        if candidate is None:
            raise ValueError(f"rating references an unknown candidate label: {path}")
        point_index = _require_integer(entry.get("reviewPointIndex"), "reviewPointIndex", 1, MAX_REVIEW_POINT_INDEX)
        identity = (label, point_index)
        if identity in seen_entries:
            raise ValueError(f"duplicate rating for one candidate and review point: {path}")
        seen_entries.add(identity)

        point_type = entry.get("pointType")
        if not isinstance(point_type, str) or point_type not in {"frame", "transition"}:
            raise ValueError(f"pointType must be frame or transition: {path}")
        start = _parse_time(entry.get("relativeTime"), "relativeTime")
        raw_end = entry.get("endRelativeTime")
        end = _parse_time(raw_end, "endRelativeTime") if raw_end is not None else None
        if (point_type == "transition") != (end is not None) or (end is not None and end <= start):
            raise ValueError(f"transition time range does not match pointType: {path}")
        definition = (point_type, start, end)
        previous_definition = point_definitions.setdefault(point_index, definition)
        if previous_definition != definition:
            raise ValueError(f"candidates do not share the same timestamp for review point {point_index}: {path}")
        vote_identity = (str(candidate["sha256"]), point_type, start, end)
        if vote_identity in seen_candidate_points:
            raise ValueError(f"duplicate vote for one candidate and timestamp: {path}")
        seen_candidate_points.add(vote_identity)

        score = entry.get("naturalnessScore")
        unable = entry.get("unableToJudge")
        if type(unable) is not bool:
            raise ValueError(f"unableToJudge must be a boolean: {path}")
        if score is None:
            if not unable:
                raise ValueError(f"a missing naturalness score must be marked unableToJudge: {path}")
        else:
            score = _require_integer(score, "naturalnessScore", 1, 5)
            if unable:
                raise ValueError(f"unableToJudge cannot be true with a numeric score: {path}")
        artifact = entry.get("observedArtifact")
        if not isinstance(artifact, str) or artifact not in ARTIFACT_CODES:
            raise ValueError(f"unsupported observedArtifact code: {path}")
        raw_confidence = entry.get("confidenceScore")
        confidence = None if raw_confidence is None else _require_integer(raw_confidence, "confidenceScore", 1, 5)
        note = _require_note(entry["note"]) if "note" in entry else None

        normalized.append({
            "sessionId": session_id,
            "sha256": candidate["sha256"],
            "name": candidate["name"],
            "pointType": point_type,
            "relativeTime": start,
            "endRelativeTime": end,
            "naturalnessScore": score,
            "confidenceScore": confidence,
            "unableToJudge": unable,
            "observedArtifact": artifact,
            "note": note,
        })
    return session_id, seed_present, source_order_complete, reviewer_code, by_label, normalized


def _parse_reviewer_codes(values: list[str]) -> dict[str, str]:
    result: dict[str, str] = {}
    for value in values:
        session_id, separator, code = value.partition("=")
        if not separator or not SESSION_ID_PATTERN.fullmatch(session_id):
            raise ValueError("--reviewer-code must use SESSION_ID=CODE with a 32-character session ID")
        if not REVIEWER_CODE_PATTERN.fullmatch(code):
            raise ValueError("reviewer codes must be 1–32 letters, digits, underscores or hyphens")
        if session_id in result:
            raise ValueError(f"reviewer code supplied more than once for session {session_id}")
        result[session_id] = code.upper()
    return result


def aggregate(paths: list[Path], reviewer_codes: dict[str, str] | None = None) -> dict[str, object]:
    if not paths or len(paths) > MAX_REVIEW_SESSIONS:
        raise ValueError(f"provide between 1 and {MAX_REVIEW_SESSIONS} review exports")
    reviewer_codes = reviewer_codes or {}
    resolved_reviewer_codes: dict[str, str] = {}
    session_ids: set[str] = set()
    seed_sessions = 0
    source_order_sessions = 0
    candidate_names: dict[str, set[str]] = defaultdict(set)
    grouped: dict[str, dict[tuple[str, Fraction, Fraction | None], list[dict[str, object]]]] = defaultdict(
        lambda: defaultdict(list))
    total_ratings = 0
    total_note_bytes = 0
    reviewer_vote_identities: set[tuple[str, str, str, Fraction, Fraction | None]] = set()

    for path in paths:
        session_id, seed_present, source_order_complete, embedded_code, mapping, ratings = _load_session(path)
        if session_id in session_ids:
            raise ValueError(f"provide only one final revealed export per session; duplicate session ID: {session_id}")
        session_ids.add(session_id)
        supplied_code = reviewer_codes.get(session_id)
        if supplied_code is not None and embedded_code is not None and supplied_code != embedded_code:
            raise ValueError(f"command-line reviewer code conflicts with the code embedded in session {session_id}")
        reviewer_code = supplied_code or embedded_code
        if reviewer_code is not None:
            resolved_reviewer_codes[session_id] = reviewer_code
        seed_sessions += int(seed_present)
        source_order_sessions += int(source_order_complete)
        total_ratings += len(ratings)
        if total_ratings > MAX_TOTAL_RATINGS:
            raise ValueError(f"total ratings exceed {MAX_TOTAL_RATINGS}")
        total_note_bytes += sum(len(str(rating["note"]).encode("utf-8")) for rating in ratings if rating["note"] is not None)
        if total_note_bytes > MAX_TOTAL_NOTE_BYTES:
            raise ValueError(f"total review notes exceed {MAX_TOTAL_NOTE_BYTES} bytes")
        for candidate in mapping.values():
            candidate_names[str(candidate["sha256"])].add(str(candidate["name"]))
        for rating in ratings:
            digest = str(rating["sha256"])
            point_type = str(rating["pointType"])
            start = rating["relativeTime"]
            end = rating["endRelativeTime"]
            if reviewer_code is not None:
                reviewer_identity = (reviewer_code, digest, point_type, start, end)
                if reviewer_identity in reviewer_vote_identities:
                    raise ValueError(f"reviewer code {reviewer_code} has multiple votes for one candidate and timestamp")
                reviewer_vote_identities.add(reviewer_identity)
            rating["reviewerCode"] = reviewer_code
            key = (point_type, start, end)
            grouped[digest][key].append(rating)

    unknown_session_ids = set(reviewer_codes) - session_ids
    if unknown_session_ids:
        raise ValueError(f"reviewer codes reference unknown session IDs: {', '.join(sorted(unknown_session_ids))}")

    candidates = []
    for digest in sorted(grouped):
        point_summaries = []
        point_groups = sorted(grouped[digest].items(), key=lambda item: (item[0][1], item[0][0], item[0][2] or Fraction(-1)))
        for (point_type, start, end), votes in point_groups:
            scores = [int(vote["naturalnessScore"]) for vote in votes if vote["naturalnessScore"] is not None]
            confidences = [int(vote["confidenceScore"]) for vote in votes if vote["confidenceScore"] is not None]
            score_counts = {str(score): scores.count(score) for score in range(1, 6)}
            confidence_counts = {str(score): confidences.count(score) for score in range(1, 6)}
            artifact_counts = {code: sum(vote["observedArtifact"] == code for vote in votes) for code in ARTIFACT_CODES}
            reviewer_votes = [
                {
                    "reviewSessionId": vote["sessionId"],
                    "reviewerCode": vote["reviewerCode"],
                    "naturalnessScore": vote["naturalnessScore"],
                    "unableToJudge": vote["unableToJudge"],
                    "confidenceScore": vote["confidenceScore"],
                    "observedArtifact": vote["observedArtifact"],
                    "note": vote["note"],
                }
                for vote in sorted(
                    votes,
                    key=lambda item: (
                        item["reviewerCode"] is None,
                        str(item["reviewerCode"] or ""),
                        str(item["sessionId"]),
                    ),
                )
            ]
            note_count = sum(vote["note"] is not None for vote in votes)
            point = {
                "pointType": point_type,
                "relativeTime": str(start),
                "endRelativeTime": str(end) if end is not None else None,
                "reviewSessionCount": len(votes),
                "reviewerCodeCount": len({vote["reviewerCode"] for vote in votes if vote["reviewerCode"] is not None}),
                "unassignedReviewerSessionCount": sum(vote["reviewerCode"] is None for vote in votes),
                "naturalness": {
                    "ratedSessionCount": len(scores),
                    "unableToJudgeSessionCount": len(votes) - len(scores),
                    "scoreCounts": score_counts,
                    "mean": math.fsum(scores) / len(scores) if scores else None,
                    "median": statistics.median(scores) if scores else None,
                },
                "confidence": {
                    "ratedSessionCount": len(confidences),
                    "scoreCounts": confidence_counts,
                    "mean": math.fsum(confidences) / len(confidences) if confidences else None,
                    "median": statistics.median(confidences) if confidences else None,
                },
                "reviewNoteCount": note_count,
                "reviewerVotes": reviewer_votes,
                "artifactCounts": artifact_counts,
                "obviousArtifactSessionCount": len(votes) - artifact_counts["none_obvious"],
                "obviousArtifactShare": (len(votes) - artifact_counts["none_obvious"]) / len(votes),
            }
            point_summaries.append(point)
        candidates.append({
            "outputSha256": digest,
            "names": sorted(candidate_names[digest]),
            "reviewedPointCount": len(point_summaries),
            "points": point_summaries,
        })

    return {
        "schemaVersion": 1,
        "application": "RifeLove",
        "recordType": "aggregated_human_visual_review",
        "aggregatedAtUnixMs": int(time.time() * 1000),
        "reviewSessionCount": len(session_ids),
        "reviewerCodeCount": len(set(resolved_reviewer_codes.values())),
        "reviewerCodeSessionCount": sum(session_id in resolved_reviewer_codes for session_id in session_ids),
        "unassignedReviewerSessionCount": sum(session_id not in resolved_reviewer_codes for session_id in session_ids),
        "reviewerCodeAssignments": [
            {"reviewSessionId": session_id, "reviewerCode": resolved_reviewer_codes[session_id]}
            for session_id in sorted(resolved_reviewer_codes)
        ],
        "uniqueHumanReviewerCount": None,
        "ratingCount": total_ratings,
        "randomizationAudit": {
            "sessionsWithSeed": seed_sessions,
            "sessionsWithoutSeed": len(session_ids) - seed_sessions,
            "sessionsWithOriginalOrder": source_order_sessions,
            "sessionsWithoutOriginalOrder": len(session_ids) - source_order_sessions,
        },
        "candidates": candidates,
        "limitations": [
            "A review session is not proof of a distinct human reviewer.",
            "Adjacent points from one clip are correlated; summaries remain per point and are not pooled into a candidate ranking.",
            "Naturalness and artifact tallies are descriptive and do not establish statistical significance.",
            "Free-text notes may reveal candidate identity or personal information.",
            "Reviewer codes are user-supplied pseudonyms and are not verified human identities.",
        ],
    }


def write_output(path: Path, document: dict[str, object], inputs: list[Path], force: bool) -> None:
    destination = path.expanduser().absolute()
    if not destination.parent.is_dir():
        raise ValueError(f"output directory does not exist: {destination.parent}")
    if destination.is_symlink():
        raise ValueError("output path must not be a symbolic link")
    destination_key = os.path.normcase(str(destination.resolve(strict=False)))
    if any(os.path.normcase(str(item.resolve())) == destination_key for item in inputs):
        raise ValueError("output path cannot overwrite an input review export")
    if destination.exists() and not force:
        raise ValueError("output already exists; pass --force to replace it")
    if destination.exists() and not destination.is_file():
        raise ValueError("output path must be a regular file")

    serialized = json.dumps(document, ensure_ascii=False, indent=2, sort_keys=True) + "\n"
    descriptor, temporary_name = tempfile.mkstemp(prefix=f".{destination.name}.", suffix=".tmp", dir=destination.parent)
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8", newline="\n") as stream:
            stream.write(serialized)
            stream.flush()
            os.fsync(stream.fileno())
        if force:
            if destination.exists() and destination.is_symlink():
                raise ValueError("output path became a symbolic link before replacement")
            os.replace(temporary, destination)
        else:
            # A same-directory hard-link publishes atomically without replacing
            # a file created by another process after the initial existence check.
            os.link(temporary, destination)
            temporary.unlink()
    finally:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reviews", nargs="+", type=Path, help="final identity-revealed review export JSON files")
    parser.add_argument("--output", required=True, type=Path, help="new aggregated JSON output file")
    parser.add_argument("--reviewer-code", action="append", default=[], metavar="SESSION_ID=CODE",
        help="optional pseudonymous reviewer code for an export; repeat for each session")
    parser.add_argument("--force", action="store_true", help="replace an existing regular output file")
    args = parser.parse_args()
    try:
        reviewer_codes = _parse_reviewer_codes(args.reviewer_code)
        document = aggregate(args.reviews, reviewer_codes)
        write_output(args.output, document, args.reviews, args.force)
    except (OSError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    print(f"Aggregated {document['reviewSessionCount']} review sessions and {document['reviewerCodeCount']} reviewer codes "
        f"across {len(document['candidates'])} output hashes.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
