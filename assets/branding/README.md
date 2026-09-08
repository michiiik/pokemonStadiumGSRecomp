# Stadium 2 launcher identity

The launcher cover uses user-supplied Pokemon Stadium 2 promotional artwork,
downscaled to 800x400 without cropping. The artwork is third-party material;
it is not covered by the project's source-code license. Redistribution rights
must be checked before publishing it. No ROM data is included.

The application icon is original gold-and-silver vector artwork. Its SVG is
the editable source. The cover SVG is an unused original-art alternative.

The cover is staged as `assets/img/boxart_stadium2.tga`. The icon is provided as
BMP for SDL windows, a multi-resolution ICO for Windows executable metadata,
and a matching Android vector resource. Existing game save/config identities
are deliberately unchanged.

To regenerate the original-art alternative, render the SVGs at their declared
sizes (640x480 and 256x256), then run
`tools/convert_launcher_branding.py` with the two rendered PNG paths. Pillow
is required only to regenerate these checked-in image formats, not to build.
This replaces the supplied promotional cover with the original-art alternative.
