# EmbedFileAsString.cmake — embed a text file as a C++ raw string constant.
#
# Usage: cmake -DINPUT_FILE=... -DOUTPUT_FILE=... -DVAR_NAME=... -DNAMESPACE=... -P EmbedFileAsString.cmake

file(READ "${INPUT_FILE}" _content)

# Vendored FDL files carry trailing whitespace on some lines; strip it so the
# generated constant matches what used to be hand-pasted into the .cc file.
string(REGEX REPLACE "[ \t]+\n" "\n" _content "${_content}")

# C++ raw string delimiters are capped at 16 characters ([lex.string]).
set(_delim "EMBED_RAW")
file(WRITE "${OUTPUT_FILE}"
"// Auto-generated from ${INPUT_FILE} — do not edit.
#pragma once

namespace ${NAMESPACE} {

inline const char ${VAR_NAME}[] = R\"${_delim}(${_content})${_delim}\";

}  // namespace ${NAMESPACE}
")
