# Initial RIFE SDR color contract

`prepareRifeSdr` and `restoreRifeSdr` form a CPU preprocessing/postprocessing boundary independent of the inference backend. This is a tested subset of the complete color pipeline, not HDR or complete SDR format acceptance.

The input must have owned, valid pixel planes with positive dimensions <=8192, 8-bit components, BT.709 primaries and an explicit BT.709 or sRGB transfer. RGB requires `gbr` matrix/full range; YUV requires BT.709 matrix and explicit full/limited range. Hardware surfaces, alpha, palette, unknown metadata, higher depth and HDR are rejected. The transfer function is preserved: these are normalized **nonlinear** RGB values, not linear-light/tone-mapped values.

libswscale is configured with explicit BT.709 coefficients and source/destination ranges. An RGB48 intermediate avoids an extra RGB8 quantization before FP32 normalization. The contiguous batch-1 NCHW buffer is padded to a multiple of 64 by repeating the last row/column. The configured float-buffer limit is checked before allocation; this limit does not cover all temporary buffers or GPU inference memory. Cancellation is checked around conversion and between rows.

Postprocessing validates finite normalized pixels, crops to the original dimensions and converts through RGB48 back to the original 8-bit layout/range/matrix. The returned frame owns its planes independently. Presentation timestamps/durations remain the timeline's responsibility. This path does not add HDR qualification to a model or silently infer missing color information.

Four unit cases check RGB channel order/edge padding/crop/ownership, limited-range black/white and tags, known BT.709 limited-range red code values (distinguishing the matrix from BT.601), and rejection of unknown color/HDR/depth/nonfinite pixels/budget/cancellation. These are numeric conversion tests; gamut/clipping policies, full chroma-position coverage, the media/model round trip, HDR/10-bit, visual quality and GPU conversion remain required.
