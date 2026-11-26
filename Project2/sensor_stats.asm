; =============================================================================
; sensor_stats.asm - Sensor data validation and statistics (x86-64, NASM)
; -----------------------------------------------------------------------------
; Linux Programming Summative - Project 2
;
; Reads a text file containing one sensor measurement per line (default:
; sensor_readings.txt), validates every record, and prints:
;
;     Total records: X
;     Valid measurements: Y
;     Invalid measurements: Z
;     Sum of valid measurements: S
;     Minimum measurement: MIN        (N/A when there are no valid values)
;     Maximum measurement: MAX        (N/A when there are no valid values)
;
; A record is VALID when it consists of an optional leading '-' followed by
; one or more decimal digits, and its value lies in the range 0..100
; inclusive ("-0" is therefore 0 and valid; "-5" is numeric but out of range).
; Everything else is INVALID: empty lines, letters, spaces, '+', '.', a '-'
; that is not the first character, values > 100, or a stray '\r' mid-line.
;
; Line endings: LF ("\n") and CRLF ("\r\n") are both accepted. A final line
; without a trailing newline is still counted; a trailing newline at the end
; of the file does NOT create an extra (empty) record.
;
; Implementation notes
;   * No C library: the program talks to the kernel directly with the
;     `syscall` instruction (open, read, write, close, exit).
;   * The file is read in 4096-byte chunks. The per-record parser state lives
;     in callee-preserved registers (r12, r13), so a record that is split
;     across two read() calls is parsed correctly and files of any size work.
;   * Global counters and statistics live in memory (.bss); the hot per-byte
;     state lives in registers.
;
; Build:   nasm -f elf64 -g -F dwarf sensor_stats.asm -o sensor_stats.o
;          ld -o sensor_stats sensor_stats.o
; Run:     ./sensor_stats                 (reads ./sensor_readings.txt)
;          ./sensor_stats other_file.txt  (optional path argument)
; Exit status: 0 success, 1 cannot open file, 2 read error.
; =============================================================================

BITS 64
default rel                     ; RIP-relative addressing for all labels

; ---- Linux x86-64 system call numbers (rax) ---------------------------------
; Arguments go in rdi, rsi, rdx, r10, r8, r9. The kernel clobbers only rax
; (return value), rcx and r11; every other register survives a syscall.
%define SYS_READ    0
%define SYS_WRITE   1
%define SYS_OPEN    2
%define SYS_CLOSE   3
%define SYS_EXIT    60

%define O_RDONLY    0
%define STDOUT      1
%define STDERR      2

%define BUF_SIZE    4096        ; bytes requested per read()
%define MIN_VALID   0           ; inclusive lower bound of a valid reading
%define MAX_VALID   100         ; inclusive upper bound of a valid reading

; ---- Per-record parser flags, kept in r13b -----------------------------------
%define F_HAS_CHAR    1         ; at least one character seen on this line
%define F_HAS_DIGIT   2         ; at least one digit seen
%define F_INVALID     4         ; an illegal character was seen
%define F_NEGATIVE    8         ; record started with '-'
%define F_PENDING_CR 16         ; last char was '\r' (expecting '\n' next)

; =============================================================================
section .data
; =============================================================================
default_path:   db "sensor_readings.txt", 0

lbl_total:      db "Total records: "
lbl_total_len   equ $ - lbl_total
lbl_valid:      db "Valid measurements: "
lbl_valid_len   equ $ - lbl_valid
lbl_invalid:    db "Invalid measurements: "
lbl_invalid_len equ $ - lbl_invalid
lbl_sum:        db "Sum of valid measurements: "
lbl_sum_len     equ $ - lbl_sum
lbl_min:        db "Minimum measurement: "
lbl_min_len     equ $ - lbl_min
lbl_max:        db "Maximum measurement: "
lbl_max_len     equ $ - lbl_max

txt_na:         db "N/A", 10    ; printed for min/max when nothing is valid
txt_na_len      equ $ - txt_na
newline:        db 10

err_open:       db "Error: cannot open file '"
err_open_len    equ $ - err_open
err_read:       db "Error: failed while reading file '"
err_read_len    equ $ - err_read
err_close:      db "Warning: failed to close file '"
err_close_len   equ $ - err_close
err_mid:        db "' (errno "
err_mid_len     equ $ - err_mid
err_end:        db ")", 10
err_end_len     equ $ - err_end

; Human-readable names for the most likely errno values.
msg_enoent:     db ": No such file or directory"
msg_enoent_len  equ $ - msg_enoent
msg_eacces:     db ": Permission denied"
msg_eacces_len  equ $ - msg_eacces
msg_eisdir:     db ": Is a directory"
msg_eisdir_len  equ $ - msg_eisdir

; =============================================================================
section .bss
; =============================================================================
alignb 16
buffer:         resb BUF_SIZE   ; file data is read into this buffer
numbuf:         resb 24         ; scratch space for integer -> ASCII
numbuf_end:

; Statistics (64-bit, so the sum can never overflow for realistic files)
total_count:    resq 1          ; every record (line)
valid_count:    resq 1
invalid_count:  resq 1
sum_valid:      resq 1          ; sum of valid readings
min_valid:      resq 1          ; smallest valid reading so far
max_valid:      resq 1          ; largest valid reading so far
file_path:      resq 1          ; pointer to the NUL-terminated path in use

; =============================================================================
section .text
global _start
; =============================================================================
;
; Register roles while scanning (callee-preserved, not touched by syscall):
;   rbx = file descriptor
;   r12 = numeric value of the current record being built
;   r13 = flags of the current record (F_* bits, low byte)
;   r14 = pointer to the next unread byte in `buffer`
;   r15 = pointer one past the last valid byte in `buffer`
; -----------------------------------------------------------------------------
_start:
    ; ---- 1. Choose the input file ------------------------------------------
    ; At process entry the stack holds: [rsp] = argc, [rsp+8] = argv[0],
    ; [rsp+16] = argv[1] ... If a path argument was given, use it; otherwise
    ; fall back to "sensor_readings.txt".
    lea     rax, [default_path]
    cmp     qword [rsp], 2          ; argc >= 2 ?
    jb      .path_chosen
    mov     rax, [rsp + 16]         ; argv[1]
.path_chosen:
    mov     [file_path], rax

    ; ---- 2. Open the file: open(path, O_RDONLY, 0) -------------------------
    mov     rdi, rax                ; arg1: pathname
    mov     esi, O_RDONLY           ; arg2: flags
    xor     edx, edx                ; arg3: mode (unused for O_RDONLY)
    mov     eax, SYS_OPEN
    syscall                         ; rax = fd, or -errno on failure
    test    rax, rax
    js      open_failed             ; negative -> error (e.g. -2 = ENOENT)
    mov     rbx, rax                ; keep the descriptor in rbx

    ; ---- 3. Initialise statistics and parser state -------------------------
    ; .bss is already zero, so only min needs a non-zero start value.
    ; min starts at the largest unsigned 64-bit value so the first valid
    ; reading always replaces it; max starts at 0 (readings are >= 0).
    mov     qword [min_valid], -1   ; 0xFFFFFFFFFFFFFFFF
    mov     qword [max_valid], 0
    xor     r12d, r12d              ; current value = 0
    xor     r13d, r13d              ; current flags = none

; ---- 4. Outer loop: refill the buffer with read() ---------------------------
read_loop:
    mov     edi, ebx                ; arg1: fd
    lea     rsi, [buffer]           ; arg2: destination buffer
    mov     edx, BUF_SIZE           ; arg3: max bytes
    mov     eax, SYS_READ
    syscall                         ; rax = bytes read, 0 = EOF, <0 = error
    test    rax, rax
    js      read_failed
    jz      end_of_file
    lea     r14, [buffer]           ; r14 = cursor
    lea     r15, [buffer]           ; r15 = end of the data just read
    add     r15, rax                ;       (buffer + bytes_read)

; ---- 5. Inner loop: traverse the buffer one character at a time -------------
scan_loop:
    cmp     r14, r15                ; consumed every byte of this chunk?
    jae     read_loop               ; yes -> read the next chunk
    movzx   eax, byte [r14]         ; al = current character (zero-extended)
    inc     r14

    ; A '\r' is only legal immediately before '\n' (CRLF). If the previous
    ; character was '\r' and this one is not '\n', that '\r' was a stray
    ; control character inside the record -> the record is invalid.
    test    r13b, F_PENDING_CR
    jz      .classify
    cmp     al, 10
    je      .classify
    and     r13b, ~F_PENDING_CR
    or      r13b, F_INVALID | F_HAS_CHAR

.classify:
    cmp     al, 10                  ; '\n' -> end of the current record
    je      .end_of_line
    cmp     al, 13                  ; '\r' -> possibly half of a CRLF
    je      .carriage_return
    cmp     al, '-'
    je      .minus_sign
    cmp     al, '0'                 ; outside '0'..'9' -> not a digit
    jb      .bad_char
    cmp     al, '9'
    ja      .bad_char

    ; ---- Digit: value = value * 10 + (char - '0') --------------------------
    or      r13b, F_HAS_DIGIT | F_HAS_CHAR
    cmp     r12, MAX_VALID          ; already above the valid range?
    ja      scan_loop               ;   then stop accumulating: the record
                                    ;   is out of range anyway, and this
                                    ;   prevents 64-bit overflow on input
                                    ;   such as 99999999999999999999999.
    imul    r12, r12, 10            ; shift existing digits one decimal place
    sub     eax, '0'                ; ASCII code -> digit value 0..9
    add     r12, rax
    jmp     scan_loop

.minus_sign:
    ; '-' is only meaningful as the very first character of a record.
    test    r13b, F_HAS_CHAR
    jnz     .bad_char
    or      r13b, F_NEGATIVE | F_HAS_CHAR
    jmp     scan_loop

.carriage_return:
    ; Remember the '\r'; it is either completed by '\n' (CRLF line end), or
    ; flagged as a stray character by the check at the top of the loop.
    or      r13b, F_PENDING_CR
    jmp     scan_loop

.bad_char:
    ; Letters, spaces, '.', '+', a misplaced '-', other control chars...
    or      r13b, F_INVALID | F_HAS_CHAR
    jmp     scan_loop

.end_of_line:
    call    finalize_record         ; classify + update statistics, reset
    jmp     scan_loop

; ---- 6. End of file ----------------------------------------------------------
end_of_file:
    ; If the last line had content but no trailing '\n', it has not been
    ; counted yet. A file ending in '\n' leaves the flags clear, so no
    ; phantom empty record is created.
    test    r13b, F_HAS_CHAR | F_PENDING_CR
    jz      .close_file
    call    finalize_record

.close_file:
    mov     edi, ebx
    mov     eax, SYS_CLOSE
    syscall
    test    rax, rax
    jns     .report
    ; close() failing is not fatal for the results: warn and continue.
    lea     r8, [err_close]
    mov     r9d, err_close_len
    call    report_error

.report:
    call    print_results

    mov     eax, SYS_EXIT
    xor     edi, edi                ; exit status 0
    syscall

; =============================================================================
; finalize_record - decide whether the record just ended is valid and update
; the statistics. Input: r12 = value, r13b = flags. Resets r12/r13 to 0.
; =============================================================================
finalize_record:
    inc     qword [total_count]     ; every line is a record

    test    r13b, F_HAS_DIGIT       ; empty line, or only '-' / junk?
    jz      .invalid
    test    r13b, F_INVALID         ; any illegal character?
    jnz     .invalid
    cmp     r12, MAX_VALID          ; value above the upper bound?
    ja      .invalid
    test    r13b, F_NEGATIVE        ; negative number?
    jz      .valid
    test    r12, r12                ;   "-0" is zero -> still valid,
    jnz     .invalid                ;   any other negative is < MIN_VALID

.valid:
    inc     qword [valid_count]
    add     [sum_valid], r12        ; sum += value

    mov     rax, [min_valid]        ; min = (value < min) ? value : min
    cmp     r12, rax
    cmovb   rax, r12                ; branch-free conditional move
    mov     [min_valid], rax

    mov     rax, [max_valid]        ; max = (value > max) ? value : max
    cmp     r12, rax
    cmova   rax, r12
    mov     [max_valid], rax
    jmp     .reset

.invalid:
    inc     qword [invalid_count]

.reset:
    xor     r12d, r12d              ; start the next record from scratch
    xor     r13d, r13d
    ret

; =============================================================================
; print_results - print the six statistics lines in the required format.
; =============================================================================
print_results:
    lea     rsi, [lbl_total]
    mov     edx, lbl_total_len
    mov     rax, [total_count]
    call    print_line_number

    lea     rsi, [lbl_valid]
    mov     edx, lbl_valid_len
    mov     rax, [valid_count]
    call    print_line_number

    lea     rsi, [lbl_invalid]
    mov     edx, lbl_invalid_len
    mov     rax, [invalid_count]
    call    print_line_number

    lea     rsi, [lbl_sum]
    mov     edx, lbl_sum_len
    mov     rax, [sum_valid]        ; 0 when nothing was valid
    call    print_line_number

    ; Minimum/maximum are undefined without valid data: print "N/A"
    ; instead of the internal sentinel values.
    cmp     qword [valid_count], 0
    je      .no_valid_data

    lea     rsi, [lbl_min]
    mov     edx, lbl_min_len
    mov     rax, [min_valid]
    call    print_line_number

    lea     rsi, [lbl_max]
    mov     edx, lbl_max_len
    mov     rax, [max_valid]
    call    print_line_number
    ret

.no_valid_data:
    lea     rsi, [lbl_min]
    mov     edx, lbl_min_len
    call    write_stdout
    lea     rsi, [txt_na]
    mov     edx, txt_na_len
    call    write_stdout

    lea     rsi, [lbl_max]
    mov     edx, lbl_max_len
    call    write_stdout
    lea     rsi, [txt_na]
    mov     edx, txt_na_len
    call    write_stdout
    ret

; -----------------------------------------------------------------------------
; print_line_number - write "<label><decimal value>\n" to stdout.
;   in: rsi = label, rdx = label length, rax = unsigned value
; -----------------------------------------------------------------------------
print_line_number:
    push    rax                     ; preserve the value across the write
    call    write_stdout            ; label
    pop     rax
    call    uint_to_ascii           ; -> rsi = digits, rdx = length
    call    write_stdout            ; number
    lea     rsi, [newline]
    mov     edx, 1
    call    write_stdout            ; '\n'
    ret

; -----------------------------------------------------------------------------
; uint_to_ascii - convert an unsigned 64-bit integer to decimal text.
;   in:  rax = value
;   out: rsi = pointer to first digit (inside numbuf), rdx = digit count
; Repeated division by 10: each remainder is the next digit from the right,
; so digits are stored backwards from the end of numbuf.
; -----------------------------------------------------------------------------
uint_to_ascii:
    lea     rsi, [numbuf_end]
    mov     ecx, 10
.next_digit:
    xor     edx, edx                ; rdx:rax is the 128-bit dividend
    div     rcx                     ; rax = quotient, rdx = remainder 0..9
    add     dl, '0'                 ; digit -> ASCII
    dec     rsi
    mov     [rsi], dl
    test    rax, rax                ; more digits? (also handles value 0)
    jnz     .next_digit
    lea     rdx, [numbuf_end]
    sub     rdx, rsi                ; length = end - start
    ret

; -----------------------------------------------------------------------------
; write_stdout / write_stderr / write_all - write rdx bytes from rsi.
; write() may write fewer bytes than requested, so loop until all are sent.
; -----------------------------------------------------------------------------
write_stdout:
    mov     edi, STDOUT
    jmp     write_all
write_stderr:
    mov     edi, STDERR
write_all:
.again:
    test    rdx, rdx                ; nothing (left) to write?
    jz      .done
    mov     eax, SYS_WRITE          ; write(fd=rdi, buf=rsi, count=rdx)
    syscall
    test    rax, rax
    jle     .done                   ; error: nothing sensible left to do
    add     rsi, rax                ; advance past the bytes written
    sub     rdx, rax
    jmp     .again
.done:
    ret

; -----------------------------------------------------------------------------
; strlen - length of the NUL-terminated string at rdi -> rax
; -----------------------------------------------------------------------------
strlen:
    xor     eax, eax
.scan:
    cmp     byte [rdi + rax], 0
    je      .found
    inc     rax
    jmp     .scan
.found:
    ret

; -----------------------------------------------------------------------------
; report_error - print "<prefix><path>' (errno N[: description])\n" to stderr.
;   in: r8 = prefix, r9 = prefix length, rax = negative syscall result
; -----------------------------------------------------------------------------
report_error:
    neg     rax                     ; -errno -> errno
    push    rax

    mov     rsi, r8
    mov     rdx, r9
    call    write_stderr            ; "Error: cannot open file '"

    mov     rdi, [file_path]
    call    strlen
    mov     rdx, rax
    mov     rsi, [file_path]
    call    write_stderr            ; the path

    lea     rsi, [err_mid]
    mov     edx, err_mid_len
    call    write_stderr            ; "' (errno "

    mov     rax, [rsp]
    call    uint_to_ascii
    call    write_stderr            ; the number

    pop     rax                     ; errno -> optional description
    cmp     rax, 2
    je      .enoent
    cmp     rax, 13
    je      .eacces
    cmp     rax, 21
    je      .eisdir
    jmp     .tail
.enoent:
    lea     rsi, [msg_enoent]
    mov     edx, msg_enoent_len
    jmp     .describe
.eacces:
    lea     rsi, [msg_eacces]
    mov     edx, msg_eacces_len
    jmp     .describe
.eisdir:
    lea     rsi, [msg_eisdir]
    mov     edx, msg_eisdir_len
.describe:
    call    write_stderr
.tail:
    lea     rsi, [err_end]
    mov     edx, err_end_len
    call    write_stderr            ; ")\n"
    ret

; -----------------------------------------------------------------------------
; Fatal error exits
; -----------------------------------------------------------------------------
open_failed:                        ; rax = -errno from open()
    lea     r8, [err_open]
    mov     r9d, err_open_len
    call    report_error
    mov     eax, SYS_EXIT
    mov     edi, 1                  ; exit status 1: cannot open
    syscall

read_failed:                        ; rax = -errno from read()
    lea     r8, [err_read]
    mov     r9d, err_read_len
    call    report_error
    mov     edi, ebx                ; release the descriptor before exiting
    mov     eax, SYS_CLOSE
    syscall
    mov     eax, SYS_EXIT
    mov     edi, 2                  ; exit status 2: read error
    syscall
