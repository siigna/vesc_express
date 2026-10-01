; The lisp half of the renderer calibration. See tools/img_diff.py.
;
; display-to-img is the call that matters and is easy to miss: set-active-img
; alone leaves disp-render reporting "display driver not initialized", because
; pointing at an image and registering it as the driver are two steps.
(def screen (img-buffer 'rgb888 400 200))
(set-active-img screen)
(display-to-img)

(import "../../../../../vesc_pkg/dash_p4/font/roboto-bold-18-4c.bin" 'fnt)

(def buf (img-buffer 'indexed4 200 60))
(img-clear buf)
(img-rectangle buf 0 0 100 60 1 '(filled))
(ttf-text buf 110 40 '(0 1 2 3) fnt "AB")
(disp-render buf 10 10 '(0x000000 0xFF0000 0x00FF00 0xFFFFFF))

(save-active-img "/tmp/lisp_cal.png")
(print "lisp rendered")
