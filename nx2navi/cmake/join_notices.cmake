# Writes OUT as FIRST, an empty line, then SECOND (the mod's THIRD_PARTY_NOTICES.md).
file(READ "${FIRST}" first)
file(READ "${SECOND}" second)
string(REGEX REPLACE "\n+$" "" first "${first}")
file(WRITE "${OUT}" "${first}\n\n${second}")
