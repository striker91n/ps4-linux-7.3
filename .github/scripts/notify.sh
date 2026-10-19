#!/usr/bin/env bash
set -euo pipefail

MSG="${1:?usage: notify.sh <message>}"

if [ -z "${DISCORD_WEBHOOK_URL:-}" ]; then
  echo "DISCORD_WEBHOOK_URL not set, skipping notify"
  exit 0
fi

PAYLOAD=$(jq -n --arg content "$MSG" '{content: $content}')
curl -sf -H "Content-Type: application/json" -d "$PAYLOAD" "$DISCORD_WEBHOOK_URL" >/dev/null \
  || echo "discord notify failed (non-fatal)"
