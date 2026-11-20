from pathlib import Path
import re
import html

s = Path("oshwhub.html").read_text(encoding="utf-8", errors="ignore")
for url, title in re.findall(r'data-href="([^"]+)" title="([^"]*)"', s):
    print(html.unescape(url), "|", html.unescape(title))
