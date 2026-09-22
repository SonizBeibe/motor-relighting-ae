# Task: upgrade RelightFX's normal-map generation to the hybrid SDF + face pipeline

## Context

`RelightFX` is a native After Effects plugin (`src/RelightFX/RelightFX.cpp`) that
relights 2D manga/anime line-art. It currently works and compiles: it computes a
per-pixel normal map with a hand-rolled 3x3 Sobel pass directly on the layer's
own ink-line luma, then shades it against a 3D light rig using two existing
`PF_Param_POINT_3D` parameters (`Light Position`, `Point of Interest` — same
convention as After Effects' own Light layers). Read the existing file before
changing anything; keep its parameter IDs, disk IDs, and overall `EffectMain`
structure — you are extending it, not rewriting it.

**The problem to fix:** pure edge-based relief only produces a bump where there
is an ink line. A large flat region with no internal lines (a cheek, a plain
jacket) gets zero curvature, so it never shades — the character's face reads as
completely flat except right at the outline. This was validated extensively by
hand (Python/numpy/scipy prototyping, not part of this repo) against real
reference art and against a commercial relight plugin's output video. The fix
below is the exact algorithm that was validated to work. Do not redesign it or
substitute a different technique (an AI/ML normal-estimation model was tried
and explicitly rejected: no usable pretrained weights exist under a usable
license, and generic depth models like MiDaS produce garbage on line art — do
not reintroduce that path).

## Hard constraints

- **No machine learning, no neural networks, no pretrained models, no network
  calls at runtime.** Everything below is deterministic, classical computer
  vision (thresholding, distance transforms, blurs, Sobel), run locally.
- The one exception, explained below, is a classical Haar/LBP cascade
  (`cv::CascadeClassifier` + `lbpcascade_animeface.xml`) for face *bounding
  box* detection. This is **not** a neural network: it is a 2011-era boosted
  cascade classifier, deterministic and CPU-only at runtime, with no dataset
  dependency once the (tiny, ~250KB) `.xml` file is present. Do not remove it
  in favor of "pure math" — a from-scratch version of this exact pipeline
  without face awareness was already tried and rejected for producing
  anatomically incoherent shadow shapes (shadows that read as an arbitrary
  blob rather than wrapping a face convincingly).
- Must degrade gracefully: most manga panels do **not** have a clear
  front-facing face (action poses, masked characters, monsters, silhouettes,
  face out of frame). When no face is confidently detected, fall back to the
  silhouette-only pipeline (steps 1-4 below applied to the whole character
  silhouette) with no face-specific enhancement layered on top. The plugin
  must never fail or render nothing just because no face was found.
- Split the expensive per-pixel computation from the interactive light
  response so dragging the light stays responsive: steps 1-4 below (segment,
  distance transform, height shaping, Sobel normal extraction) depend only on
  the artwork, not on light position, and must be cached; only step 5 (the
  dot product + toon shading) should re-run when just the light parameters
  change. Use After Effects' sequence data mechanism to persist a cached
  normal-map buffer (`PF_Handle`) across `PF_Cmd_RENDER` calls for the same
  effect instance, keyed off a cheap content signature of the checked-out
  input buffer (e.g. dimensions + a fast hash of the pixel data) — recompute
  the cache only when that signature changes, otherwise reuse it. Look at how
  `in_data`/`out_data`/sequence data are already threaded through
  `EffectMain` in the existing file for the idiom to extend; add
  `PF_Cmd_SEQUENCE_SETUP` / `PF_Cmd_SEQUENCE_RESETUP` / `PF_Cmd_SEQUENCE_FLATTEN` /
  `PF_Cmd_SEQUENCE_SETDOWN` handling as needed to own that cache's lifetime
  correctly (allocate in setup, dispose in setdown).

## New dependency: OpenCV

Add OpenCV (only `core`, `imgproc`, `objdetect` modules are needed) to
`src/RelightFX/Win/RelightFX.vcxproj`. Since this repo doesn't vendor
third-party SDKs, add an `OPENCV_DIR` user macro (same pattern as the existing
`AE_SDK_DIR` macro) that a local builder points at their own OpenCV install,
and reference `$(OPENCV_DIR)\include` / `$(OPENCV_DIR)\x64\vc17\lib` (adjust to
whatever your OpenCV distribution's actual layout is) in
`AdditionalIncludeDirectories` / `AdditionalLibraryDirectories` /
`AdditionalDependencies`. Leave a clear `TODO` comment there — this will be
finished/verified locally since you cannot compile or run this Windows/AE
project yourself. Do not attempt to vendor OpenCV binaries into the repo.

Also fetch and add `src/RelightFX/lbpcascade_animeface.xml` from
`https://raw.githubusercontent.com/nagadomi/lbpcascade_animeface/master/lbpcascade_animeface.xml`
(small, ~250KB, no formal license file but freely redistributed for over a
decade in countless open-source anime tools — fine for this personal-use
project). The plugin needs to load this file at runtime; add a
`PF_ADD_LAYER`-style or simple checkbox parameter is not appropriate here —
instead resolve the `.xml` path relative to the plugin's own `.aex` location
(use `in_data`'s plugin path callbacks / `AEGP` suites to get the plugin file
path, or fall back to a fixed relative path next to the plugin — check the SDK
for the idiomatic way to locate an auxiliary resource file shipped alongside
an effect; leave a `TODO` if uncertain, this will be finalized locally too).

## The pipeline

All of this operates on the 8bpc path only (mirror the existing file's
`PF_WORLD_IS_DEEP` 16bpc passthrough behavior — do not implement 16bpc for the
new pipeline either, keep it out of scope same as today).

### Step 1 — Topological segmentation (ink mask)

- Convert the layer to grayscale luma (already done in the existing file's
  luma computation — reuse the 0.299/0.587/0.114 weights).
- `cv::GaussianBlur` the luma lightly (sigma ~2px) purely to merge screentone
  / halftone dither into flat regions before thresholding — otherwise
  halftone dots get read as thousands of tiny fake ink boundaries and the
  output is noisy garbage. This blurred buffer is only used for
  thresholding, not as the height source itself.
- Threshold the blurred luma (`cv::threshold`, binary, around 0.35 on a
  0..1-normalized scale, i.e. ~89 on 0..255) to get an ink mask: `true`/`0`
  where there's solid dark ink, `false`/`255` elsewhere. Combine with the
  layer's own alpha (outside the character silhouette is also a "wall").

### Step 2 — Distance field (SDF) per enclosed region

- `cv::distanceTransform` (L2, mask size 5) on the *inverse* of the ink mask
  (i.e. distance from every non-ink pixel to the nearest ink pixel or
  silhouette edge).
- **Critical detail that was easy to get wrong:** normalize this distance
  per connected component, not by one global maximum. Label connected
  components of the non-ink region (`cv::connectedComponents`), find each
  component's own local maximum distance, and divide every pixel's distance
  by *its own component's* max. Without this, one huge region (e.g. the
  whole head) mathematically dominates and every small region (a hair
  strand) reads as flat by comparison, or vice versa depending on which way
  you get it backwards. Clamp to [0, 1].

### Step 3 — Height shaping

- Apply `pow(d, 0.6)` (or expose the exponent as a slider, see Parameters)
  to the normalized per-region distance to equalize how narrow vs. wide
  regions read, per the working reference formula.
- Apply a second, generous `cv::GaussianBlur` to this height buffer to turn
  the naturally faceted/polygonal look of raw distance-transform ridges into
  smooth organic curves. This blur radius should be a user-exposed slider
  (see Parameters) — too little and the shading looks like a low-poly model;
  too much and fine detail (individual hair strands) disappears.
- Produce **two** height layers with independently tunable strength, not one
  blended height map:
  - a **coarse** layer: same distance-transform-and-shape process but with
    the ink mask replaced by *only the outer silhouette boundary* (ignore
    all internal ink lines), giving one smooth "dome" per enclosed
    silhouette (the whole head+hair+body, or separate dome per disconnected
    silhouette piece). This is what makes hair/shoulders/clothing read as
    round instead of flat.
  - a **fine** layer: the full ink-line-based per-region process from steps
    1-2 above (hair strands, fabric folds, small internal detail).
  These are combined as normals (see Step 4), not as heights, with separate
  strength multipliers — a coarse region can be hundreds of pixels wide and
  a fine region a handful of pixels, so their raw pixel-space gradients
  differ by orders of magnitude and must be scaled independently before
  combining, not normalized together.

### Step 4 — Normal extraction (Sobel) + face-aware enhancement, then cache

- `cv::Sobel` (or `cv::Scharr`) the coarse height layer and the fine height
  layer separately to get `(dx, dy)` for each, apply each layer's own
  strength multiplier, then sum the two `(dx, dy)` slope vectors and build
  `vec3(-dx, -dy, 1.0)`, normalized. This is the "silhouette-only" normal
  map — it is always computed and is the fallback output.
- **Face enhancement (only when a face is found):** run
  `cv::CascadeClassifier::detectMultiScale` with the anime cascade on the
  equalized grayscale image. Tune `scaleFactor`/`minNeighbors`/`minSize` —
  a naive default (`scaleFactor=1.05, minNeighbors=3`) was found to miss
  faces that a finer `scaleFactor=1.02, minNeighbors=2` catches; test against
  a variety of art before picking final defaults, and expose them or at
  least document the chosen values in a comment. Expect roughly 20-30% of
  real manga panels to have no detectable face at all (action poses, masked
  characters, monsters, face out of frame) — that is normal, not a bug; just
  fall back.
  - When a face rectangle is found, fit an analytic ellipsoid to it: treat
    the face box as spanning roughly `[-1,1]` in local (u,v), compute
    `w = sqrt(max(0, 1 - u^2 - v^2))`, and use `(u, v, w)` normalized as the
    face's normal at each pixel inside the ellipse. This is a closed-form
    formula, no distance transform needed for this part.
  - Add a **nose bump**: a second, smaller ellipsoid centered slightly below
    the face box's vertical center, offset slightly toward whichever side
    the face is (roughly x = face center + 2% of face width, y = face
    center + 12% of face height in the validated prototype, but treat these
    as tunable constants), contributing an additive perturbation to
    `(u, v)` before renormalizing — this produces the small triangular
    catch-light near the nose bridge that reference art consistently shows
    and that a plain face ellipse alone does not.
  - Estimate eye centers from fixed anatomical proportions relative to the
    detected face box (roughly ±11-12% of face width from center, ~1-4% of
    face height above vertical center, validated empirically — do not try
    to derive these from pixel analysis of the art itself, that was tried
    and is unreliable across art styles). Build a small soft-edged
    "protection" falloff mask around each eye center (radial, feathered,
    not a hard-edged cutout — a hard cutout was tried and looks like a
    pasted sticker).
  - Blend: inside the face ellipse, the face-ellipsoid-plus-nose normal
    dominates; outside it, fall back to the silhouette-only normal from
    above; feather the transition across the ellipse boundary (e.g. blend
    between 75% and 115% of the ellipse's implicit radius) so there's no
    visible seam at the jawline. The fine detail layer should still
    contribute a *small* perturbation on top even inside the face (weight
    ~0.1-0.15 of its outside-the-face weight) — this is what produces
    eyebrow-cast shadows and similar small anatomical detail; weighting it
    the same as outside the face was tried and made the shadow shape read
    as an arbitrary blob instead of a face.
- Cache the resulting final normal map (packed however is convenient, e.g.
  three floats or three 8-bit channels per pixel) per the caching
  requirement above.

### Step 5 — Interactive lighting (render loop, not cached)

- Reuse the existing light-direction computation from `Light Position` /
  `Point of Interest` (already implemented).
- `dot = clamp(dot(N, L), 0, 1)`.
- Toon step: `smoothstep(threshold - eps, threshold + eps, dot)`, where
  `eps` should be small enough to read as a hard cel-shading cut by default,
  but must be a user-exposed slider (see Parameters) — reference art was
  found both with a hard binary cut (flat 2-tone manga) and with a much
  softer multi-band gradient (a full-color anime-style render), so this
  needs to support both.
- Before applying the shade, restore the *original* pixel value (bypass
  shading entirely) inside each eye's protection falloff mask from Step 4,
  cross-faded by that mask's softness — this is what keeps eye whites
  legible instead of getting crushed into the shadow tone, matching how
  reference art actually shades eyes.
- Composite: multiply the shadow term under the original artwork, then
  re-draw the original ink-line pixels (from the Step 1 mask) at their
  original value on top so ink lines are never dimmed or tinted by the
  lighting — the existing file already does something equivalent for its
  simpler pipeline; keep that behavior.
- Keep the existing `Preview Normal Map` debug checkbox working, showing the
  grayscale `nz` component (white = flat, dark = steep), same convention as
  today, but now reflecting the new cached hybrid normal map instead of the
  raw Sobel-on-artwork one.

## Parameters (add to the existing param list, keep existing ones as-is)

- `Shadow Hardness` (float slider 0-100%, maps to the toon step's `eps` —
  0% = very soft multi-band gradient, 100% = hard binary cel cut).
- `Height Blur Radius` (float slider, the Step 3 smoothing amount).
- `Height Curve Exponent` (float slider, default 0.6, the Step 3 `pow`
  exponent).
- `Coarse Detail Strength` / `Fine Detail Strength` (float sliders, the two
  independent Step 4 gradient multipliers).
- `Enable Face Detection` (checkbox, default on) — lets the user force
  silhouette-only mode for stylistic reasons or if detection misfires on a
  particular panel.
- `Eye Protection Strength` (float slider 0-100%, scales the Step 5 eye
  falloff mask's opacity so users can dial it back if it looks wrong on a
  particular character's eye style).

## What "done" looks like

- The project still compiles as a Windows/AE `.aex` (you can't verify this
  yourself — leave the code in a state that's clearly correct against the
  existing file's patterns and OpenCV's real API, and call out in your PR
  description anywhere you're unsure about an exact AE SDK call or OpenCV
  linkage detail so it can be fixed locally).
- Applying the effect to a layer with no detectable face still shades
  sensibly (silhouette dome + fine detail, no crash, no no-op).
- Applying it to a clear front-facing portrait shows: a believable rounded
  face independent of the hair/clothing silhouette's own shape, a small
  nose highlight breaking into the shadow side, eyes that stay legible on
  the shadow side, and hair/clothing that are *not* flat (unlike the
  current Sobel-only version).
- Dragging `Light Position`/`Point of Interest` around while scrubbing stays
  responsive because Steps 1-4 are cached and only Step 5 re-runs.
