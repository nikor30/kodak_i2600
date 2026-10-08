# Host-side image processing

Status: **working for color 300 dpi on the owner's unit (2026-10-08); constants come from one reference sheet.**
The scanner sends raw sensor lines (`commands.md` §8). Everything below is host work; the vendor does it
in its image library. Reference implementation: `tools/kdsprobe/kds_image.py` (numpy + PIL).

## Raw data properties (measured, color 300 dpi)
- Background (no paper): 2–5 of 255 per channel, at most 11 after 4×4 averaging.
- Black print on paper: 20–30 after 4×4 averaging, so paper is separable from background even where it is printed black.
- White office paper: about (229, 225, 245) R, G, B: uncorrected, blue-heavy, **linear** (no gamma).
- Column response is flat within about ±3 % (shading is corrected inside the scanner); pixel noise σ ≈ 6–8.
- Scale: an A4 sheet measures 2448 × 3464 raw pixels, i.e. **296.1 dpi** in both directions, not 300.
  The vendor resamples to 300 dpi (2487 × 3511); we keep the pixels and write 296.1 dpi into the file
  (page size comes out as 209.5 × 297.0 mm).
- Front and rear show the same sheet with opposite skew sign and mirrored horizontal position, as expected
  for a true view from each side: both sides are readable as sent (verified with a sheet printed on both sides): no flip or mirror is needed.

## Steps
1. **Split** each side's stream into pages at the trailers; pair front/rear by the image number in the trailer.
2. **Sheet mask**: maximum of R, G, B, averaged over 4×4 pixels, > 14.
3. **Blank check** (before the expensive steps): colour-correct the 4×4 averages inside the sheet, ignore a 3 %
   border, count cells more than 40 levels darker than the median. Printed pages measured 4–34 %, empty backs
   ≤ 0.001 %; a side under 0.05 % is blank.
4. **Skew**: first/last paper position per column and per row give four edges; each gets a robust line fit
   (median slope, then least squares on points within 2 cells; the outer 5 % at each end and points on the
   image border are ignored). The skew is the inlier-weighted median of the four slopes. Seen: 0.0–1.0°.
5. **Deskew + crop**: rotate (bicubic) if |skew| ≥ 0.15°, then keep the rows/columns that are more than half
   paper, minus 3 pixels on each side.
6. **Colour**: `out = 255 · clip(M · raw/255 + o)^(1/1.6)` with
   `M = [[2.734, −1.091, −0.309], [−0.294, 1.694, −0.036], [−0.025, −0.110, 1.377]]`, `o = (−0.037, −0.040, −0.041)`.
   Least-squares fit of our raw front page against the vendor driver's output of the same sheet (16× reduced,
   unclipped pixels): mean absolute error 5 of 255 after processing. Paper white clips to 255 as in the vendor output.
7. JPEG (quality 85) per page, `img2pdf` for the PDF.

## Open
- Constants are from one sheet and the front camera only; the rear camera may need its own matrix.
- Gray and black/white output, other resolutions, long or narrow sheets, sheets wider than the sensor.
- Speed on the Pi 4: ~1.6 s for a blank side, ~3.3 s for a straight page, ~7 s for a page that needs rotating
  (three in parallel); 3 duplex sheets in 15 s. The rotation dominates.

## Simplified processing in the C backend (`backend/`, 2026-10-08)
The SANE backend does steps 2, 5 (crop only) and 6 and leaves the rest to the frontend:
- **Crop without deskew**: a 4×4 cell is paper if the mean of max(R, G, B) is > 14. A cell row counts
  if at least 10 % of its cells are paper, a cell column likewise; the page is the longest
  unbroken run of counted columns by the longest unbroken run of counted rows (both cameras show
  single bright cell columns near the sensor edges, e.g. at x ≈ 40, which a plain bounding box would
  include). A skewed sheet keeps black wedges at its edges.
- **Gray** = 0.299 R + 0.587 G + 0.114 B of the colour-corrected pixel (choice of the backend, not
  compared with the vendor's gray output).
- **Black/white**: gray < threshold is black; default threshold 200 (the value that kept light gray
  print legible on the reference page).
