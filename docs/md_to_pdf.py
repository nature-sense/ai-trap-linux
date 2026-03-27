#!/usr/bin/env python3
"""Convert a Markdown file to PDF via HTML + Chromium headless."""

import sys
import os
import subprocess
import tempfile
import markdown

def md_to_pdf(md_path, pdf_path):
    with open(md_path, 'r') as f:
        md_content = f.read()

    html_body = markdown.markdown(
        md_content,
        extensions=['tables', 'fenced_code', 'toc']
    )

    html = f"""<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<style>
  body {{
    font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Helvetica, Arial, sans-serif;
    font-size: 13px;
    line-height: 1.6;
    color: #24292e;
    max-width: 900px;
    margin: 0 auto;
    padding: 20px 40px;
  }}
  h1 {{ font-size: 2em; border-bottom: 2px solid #eaecef; padding-bottom: 0.3em; margin-top: 1em; }}
  h2 {{ font-size: 1.5em; border-bottom: 1px solid #eaecef; padding-bottom: 0.2em; margin-top: 1.5em; }}
  h3 {{ font-size: 1.2em; margin-top: 1.2em; }}
  h4 {{ font-size: 1em; margin-top: 1em; }}
  code {{
    font-family: 'SFMono-Regular', Consolas, 'Liberation Mono', Menlo, monospace;
    font-size: 11px;
    background: #f6f8fa;
    padding: 0.2em 0.4em;
    border-radius: 3px;
  }}
  pre {{
    background: #f6f8fa;
    padding: 12px 16px;
    border-radius: 6px;
    overflow-x: auto;
    font-size: 11px;
    line-height: 1.5;
  }}
  pre code {{
    background: none;
    padding: 0;
  }}
  table {{
    border-collapse: collapse;
    width: 100%;
    margin: 1em 0;
    font-size: 12px;
  }}
  th, td {{
    border: 1px solid #dfe2e5;
    padding: 6px 12px;
    text-align: left;
  }}
  th {{
    background: #f6f8fa;
    font-weight: 600;
  }}
  tr:nth-child(even) {{ background: #fafbfc; }}
  blockquote {{
    border-left: 4px solid #dfe2e5;
    padding: 0 1em;
    color: #6a737d;
    margin: 0;
  }}
  hr {{ border: none; border-top: 1px solid #eaecef; margin: 2em 0; }}
  a {{ color: #0366d6; text-decoration: none; }}
  ul, ol {{ padding-left: 2em; }}
  li {{ margin: 0.25em 0; }}
  strong {{ font-weight: 600; }}
</style>
</head>
<body>
{html_body}
</body>
</html>"""

    with tempfile.NamedTemporaryFile(mode='w', suffix='.html', delete=False) as f:
        f.write(html)
        tmp_html = f.name

    try:
        result = subprocess.run([
            'chromium',
            '--headless=new',
            '--no-sandbox',
            '--disable-gpu',
            '--disable-dev-shm-usage',
            f'--print-to-pdf={pdf_path}',
            '--print-to-pdf-no-header',
            f'file://{tmp_html}'
        ], capture_output=True, text=True, timeout=30)

        if result.returncode != 0:
            print(f"Chromium error: {result.stderr}", file=sys.stderr)
            sys.exit(1)
    finally:
        os.unlink(tmp_html)

    print(f"PDF written to: {pdf_path}")

if __name__ == '__main__':
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} input.md output.pdf")
        sys.exit(1)
    md_to_pdf(sys.argv[1], sys.argv[2])
