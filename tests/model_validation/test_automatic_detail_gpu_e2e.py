import pytest

from tools.validation.automatic_detail_gpu_e2e import compare_decoded_frames


def test_resume_pixel_comparison_is_bounded_and_reports_non_exact_results():
    source = bytes(20_000)
    altered = bytearray(source)
    altered[3] = 1
    result = compare_decoded_frames([source], [bytes(altered)])
    assert result["exact"] is False
    assert result["changed_channels"] == 1
    assert result["maximum_channel_delta"] == 1
    assert compare_decoded_frames([source], [source])["exact"] is True
    altered[3] = 4
    with pytest.raises(ValueError, match="tolerance"):
        compare_decoded_frames([source], [bytes(altered)])
    altered[3] = 1
    altered[4] = altered[5] = 1
    with pytest.raises(ValueError, match="tolerance"):
        compare_decoded_frames([source], [bytes(altered)])
    with pytest.raises(ValueError, match="extents"):
        compare_decoded_frames([source], [source[:-1]])
