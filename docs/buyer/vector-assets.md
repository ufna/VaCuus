# SVG images and Lottie animations

SVG and Lottie are built into VaCuus. No additional Unreal plugin or runtime DLL is
required. In the editor, run the included demo with `vacuus.VectorDemo`.

Use an SVG element for an external file or inline SVG markup:

```html
<svg class="icon" src="icons/check.svg"></svg>
<svg class="icon" width="32" height="32" viewBox="0 0 32 32">
  <circle cx="16" cy="16" r="12" fill="#ffb43c" />
</svg>
```

An SVG can also be an RCSS decorator:

```css
.icon { display: inline-block; width: 32px; height: 32px; }
.status { decorator: svg("icons/check.svg"); }
```

Use `<svg>` or the SVG decorator for vector files; `<img>` continues to use the
raster image loader. SVG contents are parsed by LunaSVG and are not individual RmlUi
DOM nodes. Replace inline SVG using `SetInnerRML` / `innerHTML`, rather than appending
RmlUi elements for its paths and circles. The optional Preact workflow can use the
file form through `h('svg', { src: 'icons/check.svg', class: 'icon' })`.

Lottie uses Bodymovin JSON exported from After Effects:

```html
<lottie class="loading" src="animations/loading.json"></lottie>
```

```css
.loading { display: inline-block; width: 128px; height: 128px; }
```

Playback loops automatically. Format support follows rlottie, which implements a
subset of After Effects features; preview the actual exported animation in VaCuus.
The upstream element does not expose browser Lottie-player playback controls.

Files belong under the same `Content/DevUI` roots as RML. Paths are relative to the
owning document. External SVG images are relative to their SVG file; images inside
inline SVG are relative to the RML document. Lottie image assets must remain inside
the JSON file's directory or its subdirectories. PNG/JPEG image assets and embedded
data images are supported. SVG files, Lottie JSON and their images are included in
the cooked bundle; development builds also stage the loose files. A consuming plugin
uses `VaCuus.StageDevUI` as described in [setup](setup.md).

Changing an SVG or JSON file triggers the editor's full document reload. SVG cache
identities include the file contents, so another view retaining an older document
does not prevent a reload from showing the new version. The existing raster-image
live-reload limitation still applies to a PNG/JPEG edited on its own.

SVG text uses the shipped LatoLatin face, also registered as `serif` and `sans-serif`.
The SVG font cache is separate from RmlUi's text engine; an RML `@font-face` declaration
does not register a face for SVG text. Convert custom-font SVG text to paths for
portable artwork. VaCuus does not scan system font directories.

Both formats rasterize on the dedicated UI thread and produce premultiplied RGBA
textures. Static SVG reuses its bitmap. Lottie generates a replacement texture when
its animation frame changes; the previous texture is retired through the normal
renderer resource stream. Raster dimensions are limited to 4096 pixels per axis.
Zero-sized elements generate no bitmap, and invalid frame rate/duration is refused.

Budget animations by their raster size and complexity. One changed 256×256 RGBA frame
is 0.25 MiB, and 512×512 is 1 MiB before GPU upload overhead. The automation check
`VaCuus.Vector.LottieCost` measures UI update, CPU rasterization and recording for a
simple opacity animation at three sizes. Its output is a measurement for that fixture,
not a time limit or a promise for a more complex composition.

The vendored implementations are LunaSVG 3.5.0, PlutoVG 1.3.3 and a pinned rlottie
snapshot. Their source selections, upstream commits and local integration patches
are recorded under `Source/ThirdParty/`; licences are listed in
`THIRD-PARTY-NOTICES.md`.
