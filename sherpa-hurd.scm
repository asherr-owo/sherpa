;;; sherpa.scm --- GNU/Hurd-optimized SysVinit controller.

;; Copyright (C) 2026 Free Software Foundation, Inc.

;; This program is free software; you can redistribute it and/or modify
;; it under the terms of the GNU General Public License as published by
;; the Free Software Foundation; either version 3 of the License, or
;; (at your option) any later version.

(define-module (sherpa main)
  #:use-module (ice-9 ftw)
  #:use-module (ice-9 match)
  #:use-module (srfi srfi-1)
  #:export (%fifo-path))

(display "!! Director: Starting GNU/Hurd Boot Sequence !!\n")

;;; Configuration variables used by the underlying C runtime.

(define %fifo-path "/var/run/sherpa.fifo")
(define %config-directory "/etc/sherpa.d")
(define %current-runlevel "3") ; Default to multi-user text mode.

;;; Hardware Management abstraction for the Hurd.

(define (initialize-hardware)
  "Detect the host operating system and configure the GNU/Hurd translator stack."
  (let ((os-type (vector-ref (uname) 0)))
    (cond
     ((string=? os-type "GNU")
      (display "  [Hardware] GNU/Hurd detected. Setting up system translators...\n")
      ;; Ensure the master device node translator is properly set up.
      (if (file-exists? "/servers/socket/1")
          (display "  [Hardware] Standard socket servers already active.\n")
          (begin
            (display "  [Hardware] Configuring core loopback device...\n")
            ;; Example of setting a passive translator for basic networking.
            (system* "/bin/settrans" "-c" "/servers/socket/1" "/hurd/pflocal"))))
     ((string=? os-type "Linux")
      (display "  [Hardware] Warning: Running on Linux, but configured for Hurd defaults.\n")
      (if (file-exists? "/sbin/udevd")
          (system* "/sbin/udevd" "--daemon")))
     (else
      (display (format #f "  [Hardware] Running on unknown platform '~a'.\n" os-type))))))

;;; SysVinit Engine Operations.

(define (get-runlevel-directory runlevel)
  "Return the expected directory path string for a given numeric RUNLEVEL."
  (string-append %config-directory "/rc" runlevel ".d"))

(define (sysv-script-sort script-a script-b)
  "Predicate to sort SysV filename strings according to their sequence suffix."
  (string<? script-a script-b))

(define (execute-script path argument)
  "Load and execute an independent configuration script located at PATH with ARGUMENT."
  (display (format #f "  ** Executing: ~a (~a)\n" path argument))
  (catch #t
    (lambda ()
      ;; Bind the contextual execution argument so scripts can branch internal actions.
      (module-define! (current-module) '%run-action argument)
      (primitive-load path))
    (lambda (key . args)
      (display (format #f "  !! Script execution failed [~a]: ~a (~a) !!\n" path key args)))))

(define (process-runlevel runlevel)
  "Scan, sequence, and execute all controller scripts assigned to RUNLEVEL."
  (let ((dir-path (get-runlevel-directory runlevel)))
    (if (and (file-exists? dir-path)
             (eq? (stat:type (stat dir-path)) 'directory))
        (let ((entries (scandir dir-path (lambda (f) (or (string-prefix? "S" f)
                                                         (string-prefix? "K" f))))))
          (if (list? entries)
              (let* ((kill-scripts (sort (filter (lambda (f) (string-prefix? "K" f)) entries) sysv-script-sort))
                     (start-scripts (sort (filter (lambda (f) (string-prefix? "S" f)) entries) sysv-script-sort)))
                ;; In classic SysVinit, termination scripts run before start scripts.
                (for-each (lambda (f) (execute-script (string-append dir-path "/" f) 'stop)) kill-scripts)
                (for-each (lambda (f) (execute-script (string-append dir-path "/" f) 'start)) start-scripts))
              (display (format #f "!! Target runlevel directory '~a' contains no active rules !!\n" dir-path))))
        (display (format #f "!! Error: Target runlevel path '~a' missing or invalid !!\n" dir-path)))))

;;; Core execution path.

(initialize-hardware)
(display (format #f "Entering default Hurd target runlevel: ~a\n" %current-runlevel))
(process-runlevel %current-runlevel)
