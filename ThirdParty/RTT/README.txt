SEGGER RTT vendor files
=======================
Source : https://github.com/SEGGERMicro/RTT  (official SEGGER repository)
Commit : 4d8feab3150f (main branch)
Fetched: 2026-08-25 via gh api contents endpoint

Files:
  SEGGER_RTT.c            pristine (do not modify)
  SEGGER_RTT.h            pristine (do not modify)
  SEGGER_RTT_ConfDefaults.h  pristine — provides #ifndef-guarded defaults;
                          it #includes "SEGGER_RTT_Conf.h" itself (line 21),
                          so the user config is picked up automatically.
  SEGGER_RTT_Conf.h       EDITABLE — project tuning lives here only.
  LICENSE.md              upstream license

Not vendored (deliberately):
  SEGGER_RTT_Printf.c     formatting adds 2–4KB flash; this project logs via
                          SEGGER_RTT_WriteString + hand-built strings instead.
  RTT/SEGGER_RTT_ASM_ARMv7M.S   optional asm fast path, unnecessary here.

Upgrade procedure: re-fetch at new commit, diff .c/.h against these copies,
re-apply tuning in SEGGER_RTT_Conf.h only.
