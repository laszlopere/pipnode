# Bundled image-processing plugin.  Installed alongside the host binaries
# so the default plugin search path ($pkglibdir/plugins) picks it up
# without any post-install configuration.
#
# Ships a family of pure image-transform nodes, grouped into palette
# subcategories under "Image Processing": Color, Adjust (Brightness,
# Contrast, Saturation, Posterize, Solarize, Gamma, Levels, Hue Rotate,
# Vibrance, Histogram Equalize), Blur (Gaussian / Box / Motion / Median /
# Bilateral), Sharpen (Sharpen, Emboss, Edge Enhance, Unsharp Mask), Edge
# Detection (Sobel / Prewitt / Laplacian / Find Edges), two-input
# Composite blend modes (Blend, Multiply, Screen, Difference, Darken,
# Lighten, Add, Overlay, Soft Light, Hard Light, Color Dodge, Color Burn,
# Exclusion), Geometry (flip, rotate, crop, resize) and Stylize (Vignette,
# Pixelate, Add Noise, Dither, Threshold, Oil Paint, Crosshatch).  Each
# node processes the pixels in plain C against gdk-pixbuf's raw buffer; no
# external process is ever started.
#
# Core-only plugin (headless split, Phase 8.4): every node is pure logic
# with zero GTK — the only graphics dependency is gdk-pixbuf, an allowed
# core image-data dep (see lib/Makefile.am + the §6 tier classification).
# So this links libpipnode-core (not -gui) and pulls no GTK at runtime,
# letting servers install and run it under pipnode-run.  gdk-pixbuf was
# previously coming in transitively via GTK; it is now requested
# explicitly via $(GDK_PIXBUF_CFLAGS)/$(GDK_PIXBUF_LIBS).
plugindir = $(pkglibdir)/plugins

plugin_LTLIBRARIES = pipnode_image.la

pipnode_image_la_SOURCES = \
	pn-image-plugin.c \
	pn-image-ops.c \
	pn-image-ops.h \
	pn-image-grayscale.c \
	pn-image-grayscale.h \
	pn-image-black-white.c \
	pn-image-black-white.h \
	pn-image-sepia.c \
	pn-image-sepia.h \
	pn-image-invert.c \
	pn-image-invert.h \
	pn-image-brightness.c \
	pn-image-brightness.h \
	pn-image-contrast.c \
	pn-image-contrast.h \
	pn-image-saturation.c \
	pn-image-saturation.h \
	pn-image-posterize.c \
	pn-image-posterize.h \
	pn-image-solarize.c \
	pn-image-solarize.h \
	pn-image-gaussian-blur.c \
	pn-image-gaussian-blur.h \
	pn-image-box-blur.c \
	pn-image-box-blur.h \
	pn-image-motion-blur.c \
	pn-image-motion-blur.h \
	pn-image-sharpen.c \
	pn-image-sharpen.h \
	pn-image-emboss.c \
	pn-image-emboss.h \
	pn-image-edge-enhance.c \
	pn-image-edge-enhance.h \
	pn-image-median.c \
	pn-image-median.h \
	pn-image-sobel.c \
	pn-image-sobel.h \
	pn-image-prewitt.c \
	pn-image-prewitt.h \
	pn-image-laplacian.c \
	pn-image-laplacian.h \
	pn-image-find-edges.c \
	pn-image-find-edges.h \
	pn-image-blend.c \
	pn-image-blend.h \
	pn-image-multiply.c \
	pn-image-multiply.h \
	pn-image-screen.c \
	pn-image-screen.h \
	pn-image-difference.c \
	pn-image-difference.h \
	pn-image-darken.c \
	pn-image-darken.h \
	pn-image-lighten.c \
	pn-image-lighten.h \
	pn-image-add.c \
	pn-image-add.h \
	pn-image-overlay.c \
	pn-image-overlay.h \
	pn-image-soft-light.c \
	pn-image-soft-light.h \
	pn-image-hard-light.c \
	pn-image-hard-light.h \
	pn-image-color-dodge.c \
	pn-image-color-dodge.h \
	pn-image-color-burn.c \
	pn-image-color-burn.h \
	pn-image-exclusion.c \
	pn-image-exclusion.h \
	pn-image-gamma.c \
	pn-image-gamma.h \
	pn-image-levels.c \
	pn-image-levels.h \
	pn-image-hue-rotate.c \
	pn-image-hue-rotate.h \
	pn-image-vibrance.c \
	pn-image-vibrance.h \
	pn-image-hist-equalize.c \
	pn-image-hist-equalize.h \
	pn-image-unsharp-mask.c \
	pn-image-unsharp-mask.h \
	pn-image-bilateral.c \
	pn-image-bilateral.h \
	pn-image-flip-h.c \
	pn-image-flip-h.h \
	pn-image-flip-v.c \
	pn-image-flip-v.h \
	pn-image-rotate-90.c \
	pn-image-rotate-90.h \
	pn-image-rotate-270.c \
	pn-image-rotate-270.h \
	pn-image-rotate-180.c \
	pn-image-rotate-180.h \
	pn-image-rotate.c \
	pn-image-rotate.h \
	pn-image-crop.c \
	pn-image-crop.h \
	pn-image-resize.c \
	pn-image-resize.h \
	pn-image-vignette.c \
	pn-image-vignette.h \
	pn-image-pixelate.c \
	pn-image-pixelate.h \
	pn-image-noise.c \
	pn-image-noise.h \
	pn-image-dither.c \
	pn-image-dither.h \
	pn-image-threshold.c \
	pn-image-threshold.h \
	pn-image-oil-paint.c \
	pn-image-oil-paint.h \
	pn-image-crosshatch.c \
	pn-image-crosshatch.h

pipnode_image_la_CFLAGS = \
	$(GLIB_CFLAGS) \
	$(GDK_PIXBUF_CFLAGS) \
	$(JSON_GLIB_CFLAGS) \
	$(GMODULE_CFLAGS) \
	-I$(top_srcdir)/lib

pipnode_image_la_LDFLAGS = -module -avoid-version -shared

pipnode_image_la_LIBADD = \
	$(top_builddir)/lib/libpipnode-core.la \
	$(GLIB_LIBS) \
	$(GDK_PIXBUF_LIBS) \
	$(JSON_GLIB_LIBS) \
	$(GMODULE_LIBS)

pnhelpdir = $(pkgdatadir)/help
pnhelp_DATA = \
	help/PnImageGrayscale.html \
	help/PnImageBlackWhite.html \
	help/PnImageSepia.html \
	help/PnImageInvert.html \
	help/PnImageBrightness.html \
	help/PnImageContrast.html \
	help/PnImageSaturation.html \
	help/PnImagePosterize.html \
	help/PnImageSolarize.html \
	help/PnImageGaussianBlur.html \
	help/PnImageBoxBlur.html \
	help/PnImageMotionBlur.html \
	help/PnImageSharpen.html \
	help/PnImageEmboss.html \
	help/PnImageEdgeEnhance.html \
	help/PnImageMedian.html \
	help/PnImageSobel.html \
	help/PnImagePrewitt.html \
	help/PnImageLaplacian.html \
	help/PnImageFindEdges.html \
	help/PnImageBlend.html \
	help/PnImageMultiply.html \
	help/PnImageScreen.html \
	help/PnImageDifference.html \
	help/PnImageDarken.html \
	help/PnImageLighten.html \
	help/PnImageAdd.html \
	help/PnImageOverlay.html \
	help/PnImageSoftLight.html \
	help/PnImageHardLight.html \
	help/PnImageColorDodge.html \
	help/PnImageColorBurn.html \
	help/PnImageExclusion.html \
	help/PnImageGamma.html \
	help/PnImageLevels.html \
	help/PnImageHueRotate.html \
	help/PnImageVibrance.html \
	help/PnImageHistEqualize.html \
	help/PnImageUnsharpMask.html \
	help/PnImageBilateral.html \
	help/PnImageFlipH.html \
	help/PnImageFlipV.html \
	help/PnImageRotate90.html \
	help/PnImageRotate270.html \
	help/PnImageRotate180.html \
	help/PnImageRotate.html \
	help/PnImageCrop.html \
	help/PnImageResize.html \
	help/PnImageVignette.html \
	help/PnImagePixelate.html \
	help/PnImageNoise.html \
	help/PnImageDither.html \
	help/PnImageThreshold.html \
	help/PnImageOilPaint.html \
	help/PnImageCrosshatch.html

EXTRA_DIST = $(pnhelp_DATA)
