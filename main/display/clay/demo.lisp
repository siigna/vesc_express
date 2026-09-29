; Demo and regression test for clay-layout.
;
; One screen description, with no coordinates in it, laid out at two panel
; sizes. Run it on a workstation -- no hardware, no display driver:
;
;   cd main/lispBM/repl && make FEATURES="64 clay"
;   ./repl -H 400000 -M 8000000 --terminate -s ../../display/clay/demo.lisp
;
; It writes /tmp/clay_480.png and clay_800.png. The fonts it imports
; are ttf-prepare output; point them at any prepared font.
;
; The drawing half is deliberately plain lisp using img-rectangle and
; ttf-text, to show that the command list needs nothing special to render and
; that Clay itself never touches the display.

; One description, no coordinates anywhere, laid out at two panel sizes.
(import "font/roboto-bold-16-4c.bin" 'f16)
(import "font/roboto-bold-24-4c.bin" 'f24)
(import "font/roboto-bold-108-4c.bin" 'f108)

; Font table: the description refers to these by index, and so do the
; commands that come back.
(def fonts (list f16 f24 f108))
(def F16 0) (def F24 1) (def F108 2)

; indexed16, because the palette has more than four entries
(defun pill (txt) (list 'box '(h 40) '(pad 10) '(bg 1) '(radius 6)
                        (list 'text (list 'font F24) '(size 24) '(fg 3) (cons 'str txt))))

(defun cell (lbl val) (list 'col '(w grow) '(gap 2)
                            (list 'text (list 'font F16) '(size 16) '(fg 2) (cons 'str lbl))
                            (list 'text (list 'font F24) '(size 24) '(fg 3) (cons 'str val))))

(defun screen (speed)
    (list 'col '(w grow) '(h grow) '(pad 8) '(gap 8)
        (list 'row '(w grow) '(h 54) '(gap 6)
              (pill "<") (pill "LIGHT") (pill "CC 40")
              '(box (w grow) (h 1))
              (pill ">"))
        (list 'row '(w grow) '(h grow)
              (list 'text (list 'font F108) '(size 108) '(fg 3) (cons 'str speed)))
        (list 'row '(w grow) '(h 26)
              (list 'text (list 'font F24) '(size 24) '(fg 2) (cons 'str "km/h")))
        (list 'row '(w grow) '(h 44) '(bg 1) '(radius 8) '(pad 3)
              (list 'box (list 'w 260) '(h 38) '(bg 4) '(radius 6)
                    (list 'text (list 'font F24) '(size 24) '(fg 3) (cons 'str "63 %")))
              '(box (w grow) (h 1)))
        (list 'row '(w grow) '(h 96) '(gap 16)
              (cell "Range" "63.6") (cell "Trip" "18.4")
              (cell "ODO" "1243") (cell "Voltage" "58.7"))))

; indexed16 gives four 4-step ramps, so anti-aliased text and fills blend to
; their own colour instead of everything sharing indices 0..3.
;   ramp 0 (idx 0-3)  dim panel   ramp 1 (idx 4-7)   accent
;   ramp 2 (idx 8-11) text        ramp 3 (idx 12-15) green
(def pal16 '(0x000000 0x0E1013 0x191C20 0x23282E
             0x000000 0x005066 0x008CB3 0x00C8FF
             0x000000 0x646565 0xB0B1B1 0xFBFCFC
             0x000000 0x004E0D 0x00881A 0x00C321))

; description palette index -> ramp base in pal16
(defun ramp-base (i) (cond ((= i 1) 0) ((= i 2) 4) ((= i 3) 8) ((= i 4) 12) (t 0)))

(defun draw2 (cmds w h) {
        (var buf (img-buffer 'indexed16 w h))
        (img-clear buf)
        (loopforeach c cmds {
                (var k (ix c 0))
                (cond
                    ((eq k 'rect)
                        (if (> (ix c 5) 0)
                            (img-rectangle buf (ix c 1) (ix c 2) (ix c 3) (ix c 4)
                                (+ (ramp-base (ix c 5)) 3) '(filled)
                                (list 'rounded (ix c 6)))))
                    ((eq k 'text) {
                        ; Clay reports the box top; ttf-text places a baseline.
                        (var fnt (ix fonts (ix c 6)))
                        (var b (ramp-base (ix c 5)))
                        (ttf-text buf (ix c 1) (+ (ix c 2) (ttf-ascender fnt))
                            (list 0 (+ b 1) (+ b 2) (+ b 3)) fnt (ix c 7))
                    })
                    (t nil))
        })
        buf
})

(defun run (w h tag) {
        (var scr (img-buffer 'rgb888 w h))
        (set-active-img scr)
        (display-to-img)
        (disp-clear 0)
        (var t0 (systime))
        (var cmds (clay-layout (screen "42") w h fonts))
        (var dt (secs-since t0))
        (print (list tag 'commands (length cmds) 'layout-ms (* dt 1000.0)))
        (disp-render (draw2 cmds w h) 0 0 pal16)
        (save-active-img (str-merge "/tmp/clay_" tag ".png"))
})

(run 480 480 "480")
(run 800 480 "800")
(print 'done)
