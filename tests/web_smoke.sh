#!/bin/bash
# Дашборд в headless-браузере: страница рисуется, данные пришли, ни одна плитка не упала с ошибкой.
#
# Использование: tests/web_smoke.sh <путь к hostpulse> [порт]
# Нужен chromium или google-chrome.
set -euo pipefail

BIN=$1
PORT=${2:-18090}
BROWSER=$(command -v chromium || command -v chromium-browser || command -v google-chrome || true)
[ -n "$BROWSER" ] || { echo "нет chromium/google-chrome"; exit 1; }

"$BIN" --host 127.0.0.1 --port "$PORT" >/dev/null &
pid=$!
trap 'kill $pid 2>/dev/null; wait $pid 2>/dev/null || true' EXIT
for _ in $(seq 50); do curl -sf "http://127.0.0.1:$PORT/api/health" >/dev/null && break; sleep 0.1; done

profile=$(mktemp -d)
dom=$("$BROWSER" --headless=new --no-sandbox --disable-gpu --user-data-dir="$profile" \
    --virtual-time-budget=5000 --dump-dom "http://127.0.0.1:$PORT/" 2>/dev/null)
rm -rf "$profile"
# только отрисованные плитки, без текста <script>: в нём есть и 'card error', и NaN
content=$(sed -n '/id="content"/,/<script>/p' <<<"$dom" | sed '/<script>/,$d')

fail=0
ok() { echo "ok    $*"; }
bad() { echo "FAIL  $*"; fail=1; }

grep -q 'id="status">Обновлено:' <<<"$dom" && ok "данные получены" \
    || bad "статус: $(grep -o 'id="status"[^<]*' <<<"$dom")"
# плитки, которые есть на любой машине; температура, вентиляторы, сеть зависят от железа
for card in load mem disk hw; do
    grep -q "class=\"card\" id=\"$card\"" <<<"$content" && ok "плитка $card" || bad "нет плитки $card"
done
grep -q 'class="card error"' <<<"$content" && bad "ошибка отображения: $(grep -o 'class="card error"><h2>[^<]*' <<<"$content")" \
    || ok "без ошибок отображения"
grep -q 'undefined\|NaN' <<<"$content" && bad "в разметке undefined/NaN" || ok "без undefined/NaN"
exit $fail
