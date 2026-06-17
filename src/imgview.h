#pragma once

/*
 * imgview.h — the Preview window's model: decode an image file and show it.
 *
 * The decoders already exist ([png.c], [jpeg.c]) and the browser uses them
 * for inline images; this just points them at a FILE and blits the result
 * scaled-to-fit. Same idea as the text editor, but for pixels.
 */

#include <stdint.h>

struct fs_node;

/* Does this name look like an image we can open? (.png/.jpg/.jpeg) */
int         img_is_image(const char *name);

/* Decode file `n` into the viewer's buffer and show it. */
void        imgview_open(struct fs_node *n);
/* Draw the decoded image (or an error/empty message) in the rectangle. */
void        imgview_draw(int x, int y, int w, int h);
const char *imgview_name(void);

/* Draw a thumbnail of `n` fitted into maxw×maxh at (x,y); returns 1 if it
 * drew an image, 0 if `n` isn't a (decodable) image. Caches by node so it
 * only decodes when the selection changes — used by the Files preview. */
int         imgview_thumb(struct fs_node *n, int x, int y, int maxw, int maxh);
