#!/usr/bin/env bash
#
# generate-docs-index.sh — Scan markdown files for YAML frontmatter and
# generate a concise, grouped documentation index.
#
# Scans a directory for markdown files with YAML frontmatter (title,
# description, when, tags) and produces a markdown index grouped by
# subdirectory, rendered as tables.
#
# Configuration: .claude/generate-docs-index.conf (walk-forward from ~ to $PWD)
#   DOCS_DIR=docs         — Directory to scan (default: docs/)
#   OUTPUT_FILE=           — Write to file instead of stdout
#
# Usage:
#   Called by Claude Code SessionStart hook, or manually:
#   .claude/hooks/scripts/generate-docs-index.sh [docs-dir]

set -euo pipefail

# ── Configuration ────────────────────────────────────────────────────────────

DOCS_DIR="${1:-docs/}"
OUTPUT_FILE=""

# Walk-forward config: source .claude/generate-docs-index.conf from ~ to $PWD
_walk_config() {
  local target="$PWD"
  local parts=()
  local current="$HOME"

  # Only walk if PWD is under HOME
  if [[ "$target" != "$HOME"* ]]; then
    return
  fi

  # Check home dir first
  if [[ -f "$current/.claude/generate-docs-index.conf" ]]; then
    # shellcheck source=/dev/null
    source "$current/.claude/generate-docs-index.conf"
  fi

  # Walk path segments from HOME to PWD
  local relative="${target#"$HOME"}"
  relative="${relative#/}"

  IFS='/' read -ra parts <<< "$relative"
  for part in "${parts[@]}"; do
    current="$current/$part"
    if [[ -f "$current/.claude/generate-docs-index.conf" ]]; then
      # shellcheck source=/dev/null
      source "$current/.claude/generate-docs-index.conf"
    fi
  done
}

_walk_config

# Apply CLI arg override (takes precedence over config)
DOCS_DIR="${1:-$DOCS_DIR}"

# Strip trailing slash for consistent path handling
DOCS_DIR="${DOCS_DIR%/}"

if [ ! -d "$DOCS_DIR" ]; then
  echo "Error: directory '$DOCS_DIR' not found." >&2
  exit 1
fi

# ── Frontmatter parser ───────────────────────────────────────────────────────

# parse_frontmatter FILE
#
# Reads YAML frontmatter (between opening and closing ---) from a markdown
# file. Sets variables: fm_title, fm_description, fm_when, fm_tags.
# Returns 1 if no frontmatter found.
parse_frontmatter() {
  local file="$1"
  fm_title=""
  fm_description=""
  fm_when=""
  fm_tags=""

  # Check that the file starts with --- on the first line
  local first_line
  first_line=$(head -n 1 "$file")
  if [ "$first_line" != "---" ]; then
    return 1
  fi

  # Extract the frontmatter block (between first and second ---)
  local in_frontmatter=0
  local frontmatter=""
  while IFS= read -r line; do
    if [ "$in_frontmatter" -eq 0 ]; then
      if [ "$line" = "---" ]; then
        in_frontmatter=1
        continue
      else
        return 1
      fi
    fi
    if [ "$line" = "---" ]; then
      break
    fi
    frontmatter="${frontmatter}${line}
"
  done < "$file"

  if [ -z "$frontmatter" ]; then
    return 1
  fi

  # Parse individual fields from the frontmatter text.
  # Handles both quoted and unquoted values, and strips leading/trailing whitespace.
  local key value
  while IFS= read -r line; do
    # Skip empty lines and comments
    case "$line" in
      ""|\#*) continue ;;
    esac

    # Match "key: value" pattern
    key="${line%%:*}"
    value="${line#*:}"

    # Strip leading whitespace from value
    value="${value#"${value%%[![:space:]]*}"}"

    # Strip surrounding quotes (single or double)
    case "$value" in
      \"*\") value="${value#\"}"; value="${value%\"}" ;;
      \'*\') value="${value#\'}"; value="${value%\'}" ;;
    esac

    case "$key" in
      title)       fm_title="$value" ;;
      description) fm_description="$value" ;;
      when)        fm_when="$value" ;;
      tags)
        # tags: [a, b, c] -> a, b, c
        value="${value#\[}"
        value="${value%\]}"
        fm_tags="$value"
        ;;
    esac
  done <<EOF
$frontmatter
EOF

  # Must have at least a title to be useful
  if [ -z "$fm_title" ]; then
    return 1
  fi

  return 0
}

# ── Helpers ──────────────────────────────────────────────────────────────────

# Escape pipe characters so they don't break markdown tables.
escape_pipes() {
  local s="$1"
  printf '%s' "$s" | sed 's/|/\\|/g'
}

# ── Collect and group ────────────────────────────────────────────────────────

TMPDIR_WORK=$(mktemp -d)
trap 'rm -rf "$TMPDIR_WORK"' EXIT

# Find all .md files, sorted for deterministic output.
while IFS= read -r filepath; do
  relpath="${filepath#"$DOCS_DIR"/}"

  if ! parse_frontmatter "$filepath"; then
    continue
  fi

  # Determine the group (directory relative to DOCS_DIR)
  local_dir=$(dirname "$relpath")
  if [ "$local_dir" = "." ]; then
    group="(root)"
  else
    group="$local_dir"
  fi

  # Sanitize group name for use as a filename
  group_file="$TMPDIR_WORK/$(printf '%s' "$group" | sed 's/[^a-zA-Z0-9._-]/_/g')"

  # Track group ordering (first-seen order)
  if [ ! -f "$group_file" ]; then
    echo "$group" >> "$TMPDIR_WORK/_groups_order"
  fi

  # Build the table row
  title_escaped=$(escape_pipes "$fm_title")
  desc_escaped=$(escape_pipes "${fm_description:--}")
  when_escaped=$(escape_pipes "${fm_when:--}")
  tags_escaped=$(escape_pipes "${fm_tags:--}")

  printf '| [%s](%s) | %s | %s | %s |\n' \
    "$title_escaped" "$relpath" "$desc_escaped" "$when_escaped" "$tags_escaped" \
    >> "$group_file"

done < <(find "$DOCS_DIR" -name '*.md' -type f | sort)

# ── Render ───────────────────────────────────────────────────────────────────

_render() {
  echo "# Documentation Index"

  if [ ! -f "$TMPDIR_WORK/_groups_order" ]; then
    echo ""
    echo "_No markdown files with YAML frontmatter found in \`${DOCS_DIR}/\`._"
    return
  fi

  # Render groups: subdirectories alphabetically, then (root) last.
  {
    grep -v '^(root)$' "$TMPDIR_WORK/_groups_order" | sort -f || true
    grep '^(root)$' "$TMPDIR_WORK/_groups_order" || true
  } | while IFS= read -r group; do
    [ -z "$group" ] && continue

    echo ""
    echo "## ${group}"
    echo ""
    echo "| Document | Description | Read when... | Tags |"
    echo "|----------|-------------|--------------|------|"

    group_file="$TMPDIR_WORK/$(printf '%s' "$group" | sed 's/[^a-zA-Z0-9._-]/_/g')"
    sort -t'|' -k2,2 -f "$group_file"
  done

  echo ""
}

if [ -n "$OUTPUT_FILE" ]; then
  _render | tee "$OUTPUT_FILE"
else
  _render
fi
