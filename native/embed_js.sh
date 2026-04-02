#!/bin/sh
# Generate a C header with embedded JS source as a byte array.
# Usage: embed_js.sh input.js output.h var_name

INPUT="$1"
OUTPUT="$2"
VARNAME="$3"

# Get byte count (after stripping ESM export line)
NBYTES=$(sed '/^export {/d' "$INPUT" | wc -c | tr -d ' ')

cat > "$OUTPUT" << EOF
/* Auto-generated from $INPUT — do not edit */
static const unsigned char _${VARNAME}_bytes[] = {
EOF

sed '/^export {/d' "$INPUT" | xxd -i >> "$OUTPUT"

cat >> "$OUTPUT" << EOF
};
static const char *${VARNAME} = (const char *)_${VARNAME}_bytes;
static const int ${VARNAME}_len = ${NBYTES};
EOF
