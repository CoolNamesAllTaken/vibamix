# Pack vibamix.ota from slotA.bin + slotB.bin, but only once BOTH exist.
#
# Invoked (cmake -P) as an app POST_BUILD step after each slot build trailers its
# own image. A single-slot build runs this too and simply skips (the other slot's
# .bin isn't there yet); the second slot's build is the one that produces the .ota.
#
# Required -D args: A, B (slot bins), OUT (.ota), PY (python), PACK (pack_ota.py),
# VER (version override so a stale-but-mismatched pair doesn't error).

if(EXISTS "${A}" AND EXISTS "${B}")
  execute_process(
    COMMAND "${PY}" "${PACK}" "${A}" "${B}" -o "${OUT}" --version "${VER}"
    RESULT_VARIABLE _rc)
  if(NOT _rc EQUAL 0)
    message(WARNING "pack_ota: bundling ${OUT} failed (${_rc})")
  endif()
else()
  message(STATUS "pack_ota: ${OUT} not built yet — need both slotA.bin and slotB.bin "
                 "(build both slots, e.g. scripts/build_slots.sh)")
endif()
