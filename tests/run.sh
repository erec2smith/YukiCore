#!/bin/sh
BIN=${1:-./YokiCore}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
TMP=$(mktemp -d)
FAILS=0
LOG="$TMP/out.log"

pass() { echo "ok   $1"; }
fail() { echo "FAIL $1"; FAILS=$((FAILS+1)); }
check() { if eval "$2"; then pass "$1"; else fail "$1"; fi; }

cleanup() {
    [ -n "$PID" ] && kill "$PID" 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT INT TERM

YOKICORE_NO_BROWSER=1 YOKICORE_HOME="$TMP/home" "$BIN" >"$LOG" 2>&1 &
PID=$!
mkdir -p "$TMP/home"

URL=""
for i in 1 2 3 4 5 6 7 8 9 10; do
    URL=$(sed -n 's/.*running at \(http[^ ]*\).*/\1/p' "$LOG" | head -n 1)
    [ -n "$URL" ] && break
    sleep 1
done
check "server starts" '[ -n "$URL" ]'
[ -z "$URL" ] && exit 1
BASE=${URL%/}

curl -s "$BASE/" -o "$TMP/ui.html"
TOKEN=$(sed -n "s/.*const T='\([0-9a-f]*\)'.*/\1/p" "$TMP/ui.html" | head -n 1)
check "ui served with token" '[ ${#TOKEN} -eq 16 ]'

CODE=$(curl -s -o /dev/null -w '%{http_code}' -X POST --data '{}' "$BASE/export")
check "post without token is rejected" '[ "$CODE" = 403 ]'

CODE=$(curl -s -o /dev/null -w '%{http_code}' "$BASE/nothing")
check "unknown route is 404" '[ "$CODE" = 404 ]'

curl -s -X POST --data-binary @"$ROOT/tests/fixtures/project.json" "$BASE/export?t=$TOKEN" -o "$TMP/export.json"
check "export reports ok" 'grep -q "\"ok\":true" "$TMP/export.json"'
SITE="$TMP/home/YokiCore/fixture"
check "export writes both pages" '[ -f "$SITE/index.html" ] && [ -f "$SITE/about.html" ]'
check "export writes css and js" '[ -f "$SITE/css/base.css" ] && [ -f "$SITE/css/index.css" ] && [ -f "$SITE/js/script.js" ]'
check "hidden attribute is exported" 'grep -q "hidden=\"1\"" "$SITE/index.html"'
check "hidden rule is in base css" 'grep -q "\[hidden\]" "$SITE/css/base.css"'
check "custom and event js are merged" 'grep -q "window.fixture=1" "$SITE/js/script.js" && grep -q "window.evjs=1" "$SITE/js/script.js"'
check "project json is kept" '[ -f "$SITE/project.json" ]'

curl -s -X POST --data-binary @"$ROOT/tests/fixtures/project.json" "$BASE/preview?t=$TOKEN" -o "$TMP/preview.html"
check "preview renders page" 'grep -q "<button" "$TMP/preview.html" && grep -q "window.evjs=1" "$TMP/preview.html"'

curl -s -X POST --data-binary @"$ROOT/tests/fixtures/project.json" "$BASE/newtab?t=$TOKEN" -o "$TMP/newtab.json"
check "newtab returns url" 'grep -q "/v/$TOKEN/index.html" "$TMP/newtab.json"'
CODE=$(curl -s -o "$TMP/tab.html" -w '%{http_code}' "$BASE/v/$TOKEN/about.html")
check "newtab serves second page" '[ "$CODE" = 200 ] && grep -q "About page" "$TMP/tab.html"'
CODE=$(curl -s -o /dev/null -w '%{http_code}' "$BASE/v/0000000000000000/index.html")
check "newtab rejects wrong token" '[ "$CODE" = 404 ] || [ "$CODE" = 403 ]'
check "newtab pages link without postMessage" '! grep -q "postMessage" "$TMP/tab.html"'

if command -v node >/dev/null 2>&1; then
    node -e '
const s=require("fs").readFileSync(process.argv[1],"utf8");
const m=s.match(/<script>\n([\s\S]*)<\/script>/);
new Function(m[1].replace(/__YC_TOKEN__/g,"x"));
' "$ROOT/src/ui.html" 2>"$TMP/node.err"
    check "ui script parses" '[ $? -eq 0 ]'
fi

curl -s -X POST "$BASE/quit?t=$TOKEN" >/dev/null
wait "$PID"
check "server quits on request" '[ $? -eq 0 ]'
PID=""

[ "$FAILS" -eq 0 ] && echo "all passed" && exit 0
echo "$FAILS failed"
exit 1
