.intel_syntax noprefix
.code32

# Code cave base: VA 0x7826f1 (2319 bytes of confirmed zero padding at the
# end of .text). All addresses below are absolute VAs - this game has no
# relocation/ASLR, ImageBase is fixed.

.set CAVE_BASE,        0x7826f1
.set DAT_BB_WIDTH,     0x870980   # live backbuffer width (already tracks our resolution-table patch)
.set DAT_BB_HEIGHT,    0x870984   # live backbuffer height
.set PTR_DEVICE,       0x870974   # IDirect3DDevice9* global
.set SETVIEWPORT_OFF,  0xbc       # vtable offset of SetViewport
.set CLEAR_OFF,        0xac       # vtable offset of Clear
.set SETRENDERSTATE_OFF,0xe4      # vtable offset of SetRenderState
.set D3DRS_SCISSORTESTENABLE,0xae # 174 decimal
.set D3DCLEAR_TARGET,  0x1

.set RESUME_5c5b70,    0x5c5b76
.set RESUME_5c6d40,    0x5c6d46
.set RESUME_5c4c20,    0x5c4c26
.set RESUME_5d288f,    0x5d2894
.set ORIG_CALL_5d288f, 0x488760   # the call instruction our 4th hook displaces

.text
# NOTE: this file's own origin is 0 as far as the assembler is concerned.
# The real load address (CAVE_BASE) is supplied at link time via
# `ld -Ttext 0x7826f1`, so that direct jumps to the fixed absolute
# addresses below (RESUME_*) get their relative displacements computed
# correctly. Do not assemble/link this standalone at origin 0.
#
# --- Architecture ---
# An earlier version toggled the viewport (widen -> clear bars -> narrow)
# on every single hooked 2D-quad draw call - including FUN_005c4c20, which
# fires ~1900 times per menu frame. That caused two confirmed-live bugs:
#  1. D3D9's viewport is global persistent state, not scoped per draw
#     call. Hooking via `jmp` (not `call`) into each function's resume
#     point means we never regain control to restore the viewport
#     afterward - so the backbuffer was left in the *narrow* letterboxed
#     state for whatever drew next. Since 3D world geometry (car, track)
#     doesn't re-set the viewport itself (it relies on the FOV-fix code
#     having set it once), it was inheriting our leftover narrow viewport
#     and rendering squeezed into the letterboxed rect instead of filling
#     the full widescreen backbuffer - visible live as the driving view
#     being incorrectly narrowed during a race.
#  2. Doing the widen/clear-bars/narrow dance ~1900 times per frame
#     produced garbled white/black blocks in the bar regions instead of
#     clean black.
#
# First fix: split into two routines with different call frequencies -
# set_narrow_viewport (SetViewport only, no clear) before each hooked
# draw, set_full_viewport_and_clear_bars (widen + clear the two bar rects)
# once per frame plus once after a device Reset(). This fixed both
# confirmed bugs above, but a *third*, harder-to-pin-down bar-region
# artifact (a torn/garbled block in place of clean black) persisted
# regardless. Eight independent, live-verified attempts at fixing it via
# the bar-clear itself all failed to change it: correct D3DRECT values
# (byte-for-byte verified against a live 2560x1080 backbuffer), correct
# render target (2000/2000 live samples matched, so a "force/verify
# render target" fix was pure overhead), scissor state forced off,
# xmm0-7 preservation, a (wrong) heuristic to exempt full-width quads from
# letterboxing, and forced 16-byte stack alignment before every D3D9 call.
# Removing the bar-clear entirely didn't change the artifact either,
# which was the deciding clue: the bug was never in *what* the clear did,
# it was in something set_narrow_viewport itself left behind.
#
# Actual fix: set_narrow_viewport's "leave it narrow until next frame"
# behavior (point 1 above) was reasoned to be safe on the assumption nothing
# else draws between two hooked calls in the same frame - an assumption
# that doesn't hold in general (this game's own bloom/post-process pass,
# or DXVK/wined3d's internal state, can run in between). The three
# per-element hooks now use a return-address-swap trampoline
# (saved_ret_*/post_hook_* below) to restore the *wide* viewport
# immediately after each individual draw call returns, via
# set_wide_viewport, rather than leaving it narrow indefinitely. This is
# what actually resolved the artifact - confirmed live, stable across
# menu, HUD, video, and racing after this change.
#
# set_full_viewport_and_clear_bars still runs once per frame (right after
# Present(), hooked into FUN_005d24a0's existing frame-end Clear() call)
# and once after a device Reset() (FUN_005d1870's tail, since a
# compositor-triggered redraw - e.g. a workspace switch - can force a
# device-lost/Reset() cycle whose freshly recreated swapchain image(s)
# were never touched by the once-per-frame hook).
#
# Register allocation (both routines): ebx = bb_width, ecx = bb_height,
# esi = x_out, edi = width_out, persist unchanged for the whole routine.
# edx/eax are throwaway scratch ONLY - edx is deliberately *not* kept
# holding the device pointer across a SetViewport/Clear call: per the
# stdcall calling convention, only ebx/esi/edi/ebp/esp are guaranteed
# preserved by a called function - eax/ecx/edx are volatile, and
# SetViewport()/Clear() (real D3D9 methods) are free to clobber edx
# internally. Confirmed live: a real, reproducible null-pointer crash at
# the second vtable dispatch happened when edx was trusted to survive the
# first call. Fix: reload the device pointer from PTR_DEVICE fresh
# immediately before each vtable dispatch.
#
# pushad/pushfd only cover the general-purpose registers - NOT xmm0-7.
# Those are just as volatile-across-a-call as eax/ecx/edx, and this game
# (like any 2004-era title using D3DX/vector math) keeps live matrix/
# vector state in them between operations. SetViewport()/Clear() (real
# D3D9/DXVK/wined3d methods) are free to clobber xmm0-7 internally, and
# calling them from a hook that fires mid-frame - potentially while the
# game has a matrix/vector calculation in flight in those registers -
# would corrupt whatever computation was interrupted. Explicitly save and
# restore xmm0-7 (128 bytes, unaligned-safe via movdqu) around both
# routines' bodies, same as pushad covers eax..edi.
#
# Every call into a real D3D9 method (SetViewport/Clear/SetRenderState) is
# preceded by `mov ebp, esp; and esp, 0xfffffff0` and followed by
# `mov esp, ebp` to force 16-byte stack alignment at the call site, then
# restore exactly. Confirmed live: sampling 500 calls into SetViewport
# from set_narrow_viewport (before this fix) found esp mod 16 was 0 only
# ~81% of the time (403/500), with the rest off by 4 or 8 bytes -
# depending entirely on whatever alignment the game's own calling context
# happened to leave, which our hook never normalized. DXVK/wined3d are
# modern, SSE-optimized C++ codebases (unlike this 2004 game) and their
# compiled code may assume the standard x86 ABI guarantee of a 16-byte
# aligned stack at function entry for internal aligned SSE loads/stores;
# calling in misaligned is undefined behavior that wouldn't necessarily
# crash but could produce corrupted results - a plausible match for a
# torn/garbled visual artifact that persisted through every previous fix
# attempt (correct rect math, correct render target, scissor disabled,
# xmm preserved) yet vanished entirely when set_narrow_viewport's own
# calls were removed from the picture. ebp is free to use as scratch here
# since nothing else in either routine relies on it, and popad restores
# the caller's real ebp on the way out regardless of what we do to it.
#
# NOTE on things that were tried and REMOVED after live testing disproved
# them (kept here so this isn't re-attempted blind):
#  - Forcing the render target back to the backbuffer via GetBackBuffer/
#    SetRenderTarget before clearing: live-sampled 2000/2000 calls with a
#    render-target-match check and it NEVER mismatched - this was never
#    actually necessary, and SetRenderTarget has a documented side effect
#    of resetting the viewport, adding unwanted risk for no benefit.
#  - Skipping set_narrow_viewport when the bound render target isn't the
#    main backbuffer (same reasoning - the check never triggered).
#  - Skipping letterboxing for quads whose own geometry already spans the
#    full 640-wide reference canvas (x0=0, x1=640), on the theory that
#    these are atmospheric overlays meant to stay full-width: WRONG -
#    full-screen video and the menu background image are *also* naturally
#    full-width in that space (that's what "background"/"video" means),
#    so this incorrectly left real content unletterboxed (confirmed live:
#    caused video and the menu background to render stretched) without
#    fixing the actual artifact.
#  - Removing the bar-clear calls entirely: the artifact persisted anyway,
#    ruling out Clear() (and confirming, separately, that the D3DRECT
#    values passed to it were byte-for-byte correct: {0,0,560,1080} and
#    {2000,0,2560,1080} against a live-verified 2560x1080 backbuffer).
#    This was the clue that led to the actual fix (see the Architecture
#    note above): the bug lived in set_narrow_viewport leaving state
#    behind, not in the clear.
#
# One implementation pitfall hit while building the return-address-swap
# fix, worth keeping in mind for any future scratch data placed in this
# cave: writing to `.text` (where saved_ret_5c4c20 etc. below live) faulted
# with a real page-protection SIGSEGV on the very first attempt - .text is
# CODE|EXECUTE|READ, not writable, by default. `patch_letterbox_2d.py`
# flips on IMAGE_SCN_MEM_WRITE for the section as part of applying this
# patch; that's a prerequisite for the code below to work at all.

# --- set the centered 4:3 letterboxed viewport (no clear) ---
set_narrow_viewport:
    pushfd
    pushad
    sub     esp, 0x80
    movdqu  [esp+0x00], xmm0
    movdqu  [esp+0x10], xmm1
    movdqu  [esp+0x20], xmm2
    movdqu  [esp+0x30], xmm3
    movdqu  [esp+0x40], xmm4
    movdqu  [esp+0x50], xmm5
    movdqu  [esp+0x60], xmm6
    movdqu  [esp+0x70], xmm7

    # Bail out (no-op) if called before the device/backbuffer are set up -
    # confirmed live: a real, reproducible crash (page fault dereferencing
    # a null device pointer) happens if one of the hooked functions runs
    # this early.
    mov     edx, [PTR_DEVICE]
    test    edx, edx
    jz      narrow_bail

    mov     ebx, [DAT_BB_WIDTH]
    mov     ecx, [DAT_BB_HEIGHT]
    test    ebx, ebx
    jz      narrow_bail
    test    ecx, ecx
    jz      narrow_bail

    # width_out = height * 4 / 3
    mov     eax, ecx
    imul    eax, eax, 4
    cdq
    mov     edi, 3
    idiv    edi
    mov     edi, eax          # edi = width_out

    # x_out = (bb_width - width_out) / 2
    mov     esi, ebx
    sub     esi, edi
    sar     esi, 1            # esi = x_out

    mov     ebp, esp
    and     esp, 0xfffffff0
    push    0x3f800000      # MaxZ = 1.0f
    push    0                # MinZ = 0.0f
    push    ecx              # Height = bb_height
    push    edi              # Width  = width_out
    push    0                # Y = 0
    push    esi              # X = x_out
    mov     eax, esp
    push    eax
    mov     edx, [PTR_DEVICE]
    push    edx
    mov     eax, [edx]
    call    dword ptr [eax + SETVIEWPORT_OFF]
    mov     esp, ebp

narrow_bail:
    movdqu  xmm0, [esp+0x00]
    movdqu  xmm1, [esp+0x10]
    movdqu  xmm2, [esp+0x20]
    movdqu  xmm3, [esp+0x30]
    movdqu  xmm4, [esp+0x40]
    movdqu  xmm5, [esp+0x50]
    movdqu  xmm6, [esp+0x60]
    movdqu  xmm7, [esp+0x70]
    add     esp, 0x80
    popad
    popfd
    ret

# --- set the full-backbuffer viewport (no clear, no scissor touch) - the
# lightweight counterpart to set_narrow_viewport used to restore the
# viewport immediately after each individual hooked draw call finishes,
# rather than leaving it narrow until the next frame's
# set_full_viewport_and_clear_bars hook. See the trampoline_5c4c20 etc.
# comments below for why this exists: with the previous "leave it narrow
# until next frame" design, ANY draw not covered by our 3 hooks that
# happens to run between two hooked calls in the same frame would inherit
# the leftover narrow viewport and render squeezed/corrupted. This makes
# the narrow viewport scoped to the exact duration of one draw call. ---
set_wide_viewport:
    pushfd
    pushad
    sub     esp, 0x80
    movdqu  [esp+0x00], xmm0
    movdqu  [esp+0x10], xmm1
    movdqu  [esp+0x20], xmm2
    movdqu  [esp+0x30], xmm3
    movdqu  [esp+0x40], xmm4
    movdqu  [esp+0x50], xmm5
    movdqu  [esp+0x60], xmm6
    movdqu  [esp+0x70], xmm7

    mov     edx, [PTR_DEVICE]
    test    edx, edx
    jz      wide_bail

    mov     ebx, [DAT_BB_WIDTH]
    mov     ecx, [DAT_BB_HEIGHT]
    test    ebx, ebx
    jz      wide_bail
    test    ecx, ecx
    jz      wide_bail

    mov     ebp, esp
    and     esp, 0xfffffff0
    push    0x3f800000      # MaxZ = 1.0f
    push    0                # MinZ = 0.0f
    push    ecx              # Height = bb_height
    push    ebx              # Width  = bb_width
    push    0                # Y = 0
    push    0                # X = 0
    mov     eax, esp
    push    eax
    mov     edx, [PTR_DEVICE]
    push    edx
    mov     eax, [edx]
    call    dword ptr [eax + SETVIEWPORT_OFF]
    mov     esp, ebp

wide_bail:
    movdqu  xmm0, [esp+0x00]
    movdqu  xmm1, [esp+0x10]
    movdqu  xmm2, [esp+0x20]
    movdqu  xmm3, [esp+0x30]
    movdqu  xmm4, [esp+0x40]
    movdqu  xmm5, [esp+0x50]
    movdqu  xmm6, [esp+0x60]
    movdqu  xmm7, [esp+0x70]
    add     esp, 0x80
    popad
    popfd
    ret

# --- widen to the full backbuffer and clear the two letterbox bar
# regions (left/right of the centered 4:3 area) to black - see the
# architecture note above for why this runs once per frame rather than
# per draw call. D3D9's Clear() is always clipped to the *current*
# viewport (even with explicit rects), hence widening first. Scissor test
# is also forced off first, in case the game left an active (and
# narrower) scissor rect bound from its own UI clipping - Clear() is
# clipped by an active scissor rect the same way it's clipped by the
# viewport. ---
set_full_viewport_and_clear_bars:
    pushfd
    pushad
    sub     esp, 0x80
    movdqu  [esp+0x00], xmm0
    movdqu  [esp+0x10], xmm1
    movdqu  [esp+0x20], xmm2
    movdqu  [esp+0x30], xmm3
    movdqu  [esp+0x40], xmm4
    movdqu  [esp+0x50], xmm5
    movdqu  [esp+0x60], xmm6
    movdqu  [esp+0x70], xmm7

    mov     edx, [PTR_DEVICE]
    test    edx, edx
    jz      full_bail

    mov     ebp, esp
    and     esp, 0xfffffff0
    push    0                    # Value = FALSE
    push    D3DRS_SCISSORTESTENABLE
    push    edx                  # this
    mov     eax, [edx]
    call    dword ptr [eax + SETRENDERSTATE_OFF]
    mov     esp, ebp

    mov     ebx, [DAT_BB_WIDTH]
    mov     ecx, [DAT_BB_HEIGHT]
    test    ebx, ebx
    jz      full_bail
    test    ecx, ecx
    jz      full_bail

    # width_out = height * 4 / 3
    mov     eax, ecx
    imul    eax, eax, 4
    cdq
    mov     edi, 3
    idiv    edi
    mov     edi, eax          # edi = width_out

    # x_out = (bb_width - width_out) / 2
    mov     esi, ebx
    sub     esi, edi
    sar     esi, 1            # esi = x_out

    # --- full-backbuffer viewport, so the bar rects below aren't clipped ---
    mov     ebp, esp
    and     esp, 0xfffffff0
    push    0x3f800000      # MaxZ = 1.0f
    push    0                # MinZ = 0.0f
    push    ecx              # Height = bb_height
    push    ebx              # Width  = bb_width
    push    0                # Y = 0
    push    0                # X = 0
    mov     eax, esp
    push    eax
    mov     edx, [PTR_DEVICE]
    push    edx
    mov     eax, [edx]
    call    dword ptr [eax + SETVIEWPORT_OFF]
    mov     esp, ebp

    # --- clear left bar {0,0 -> x_out,bb_height} and right bar
    #     {x_out+width_out,0 -> bb_width,bb_height} to black ---
    mov     ebp, esp
    and     esp, 0xfffffff0
    lea     eax, [esi + edi]      # eax = x_out + width_out (right bar's left edge)
    push    ecx                    # right rect y2 = bb_height
    push    ebx                    # right rect x2 = bb_width
    push    0                      # right rect y1 = 0
    push    eax                    # right rect x1 = x_out + width_out
    push    ecx                    # left  rect y2 = bb_height
    push    esi                    # left  rect x2 = x_out
    push    0                      # left  rect y1 = 0
    push    0                      # left  rect x1 = 0
    mov     eax, esp                # eax = &rects[0] (2 D3DRECTs, 32 bytes)
    push    0                      # Stencil
    push    0                      # Z (unused - D3DCLEAR_TARGET only)
    push    0                      # Color = black (0x00000000)
    push    D3DCLEAR_TARGET
    push    eax                    # pRects
    push    2                      # Count
    mov     edx, [PTR_DEVICE]
    push    edx                    # this
    mov     eax, [edx]
    call    dword ptr [eax + CLEAR_OFF]
    mov     esp, ebp

full_bail:
    movdqu  xmm0, [esp+0x00]
    movdqu  xmm1, [esp+0x10]
    movdqu  xmm2, [esp+0x20]
    movdqu  xmm3, [esp+0x30]
    movdqu  xmm4, [esp+0x40]
    movdqu  xmm5, [esp+0x50]
    movdqu  xmm6, [esp+0x60]
    movdqu  xmm7, [esp+0x70]
    add     esp, 0x80
    popad
    popfd
    ret

# --- saved return-address slots for the three per-element hooks below.
# Each hook swaps the incoming return address for one of these post-hook
# labels, so that when the (unmodified) original function eventually
# executes its own `ret`, control lands back in OUR code (to restore the
# wide viewport) before finally jumping to the true original caller. Not
# reentrancy-safe (a single shared slot per hook), but these are draw
# functions called sequentially from the render thread, not recursively.
saved_ret_5c5b70:
    .long 0
saved_ret_5c6d40:
    .long 0
saved_ret_5c4c20:
    .long 0

# --- trampoline for FUN_005c5b70 ---
# original overwritten bytes: sub esp, 0x164  (6 bytes @ 0x5c5b70)
#
# Restores the wide viewport immediately after this draw call returns,
# instead of leaving it narrow until the next frame's
# set_full_viewport_and_clear_bars hook (see set_wide_viewport's comment).
trampoline_5c5b70:
    pop     eax
    mov     dword ptr [saved_ret_5c5b70], eax
    lea     eax, [post_hook_5c5b70]
    push    eax
    call    set_narrow_viewport
    sub     esp, 0x164
    jmp     RESUME_5c5b70
post_hook_5c5b70:
    call    set_wide_viewport
    jmp     dword ptr [saved_ret_5c5b70]

# --- trampoline for FUN_005c6d40 ---
# original overwritten bytes: push ebp; mov ebp,esp; and esp,0xfffffff0  (6 bytes @ 0x5c6d40)
trampoline_5c6d40:
    pop     eax
    mov     dword ptr [saved_ret_5c6d40], eax
    lea     eax, [post_hook_5c6d40]
    push    eax
    call    set_narrow_viewport
    push    ebp
    mov     ebp, esp
    and     esp, 0xfffffff0
    jmp     RESUME_5c6d40
post_hook_5c6d40:
    call    set_wide_viewport
    jmp     dword ptr [saved_ret_5c6d40]

# --- trampoline for FUN_005c4c20 ---
# the general-purpose "draw one UI element" quad function - confirmed live
# (breakpoint sweep while sitting on the static title screen) to be the
# most frequently invoked draw-capable function there. Same prologue
# pattern as FUN_005c6d40, so the same original-bytes re-execution applies.
#
# NOT menu-exclusive though: it's also used for full-screen effects (e.g.
# a race-start wipe/transition), which must NOT be letterboxed - confirmed
# live (visible glitch: the effect rendered squeezed into the shrunk
# viewport instead of spanning the real screen). Its own param_3 (third
# stack arg at entry, [esp+0xc]) distinguishes these: menu-element calls
# only ever pass 0 or 1 (verified live, ~2000 calls sampled across the
# title screen, no other value seen), while the function's own decompiled
# source shows a distinctly different code path specifically for
# param_3==0x61 - so only letterbox for param_3 in {0, 1}, leave the
# viewport untouched otherwise. When param_3 > 1, no return-address swap
# happens either - that call is left entirely alone.
# original overwritten bytes: push ebp; mov ebp,esp; and esp,0xfffffff0  (6 bytes @ 0x5c4c20)
trampoline_5c4c20:
    cmp     dword ptr [esp + 0xc], 1
    ja      skip_letterbox_5c4c20   # param_3 > 1 (unsigned): not a menu-element call, leave viewport alone
    pop     eax
    mov     dword ptr [saved_ret_5c4c20], eax
    lea     eax, [post_hook_5c4c20]
    push    eax
    call    set_narrow_viewport
skip_letterbox_5c4c20:
    push    ebp
    mov     ebp, esp
    and     esp, 0xfffffff0
    jmp     RESUME_5c4c20
post_hook_5c4c20:
    call    set_wide_viewport
    jmp     dword ptr [saved_ret_5c4c20]

# --- trampoline for the frame-end backbuffer clear in FUN_005d24a0 ---
# hooks the instruction immediately after the game's own existing
# Clear(Count=0, pRects=NULL, Flags=D3DCLEAR_TARGET|ZBUFFER|STENCIL) call
# (vtable+0xac, at VA 0x5d2889) inside the main per-frame render function
# - confirmed via decompiled source and live disassembly to run exactly
# once per frame, right after Present(), preparing the backbuffer for the
# frame about to be drawn. The displaced instruction, `call 0x488760`
# (5 bytes, no args), is re-executed verbatim after our own call.
# original overwritten bytes: call 0x488760  (5 bytes @ 0x5d288f)
trampoline_5d288f:
    call    set_full_viewport_and_clear_bars
    call    ORIG_CALL_5d288f
    jmp     RESUME_5d288f

# --- trampoline for the tail of FUN_005d1870 (device Reset() handler) ---
# hooks the function's final "restore eax/esi, ret" epilogue, reached both
# when Reset() succeeds immediately and after a successful retry - i.e.
# right after the device has actually been reset and the game's own
# post-reset re-initialization (render states, resources) has finished.
# original overwritten bytes: pop edi; mov eax,esi; pop esi; add esp,0x8
# (7 bytes @ 0x5d1a50, immediately followed by the function's own `ret`)
trampoline_5d1a50:
    call    set_full_viewport_and_clear_bars
    pop     edi
    mov     eax, esi
    pop     esi
    add     esp, 8
    ret
