#!/usr/bin/env python3
"""Generates docs/images/architecture.svg (and .png via rsvg-convert) from the logo files in docs/images/logos."""
import os, re, subprocess
here = os.path.dirname(os.path.abspath(__file__))
def embed(path, x, y, h, prefix, viewbox=None):
    """Inline an SVG file as a nested <svg>, prefixing its CSS classes/ids so several logos can coexist."""
    s = open(os.path.join(here, 'logos', path)).read()
    s = re.sub(r'<\?xml[^>]*\?>|<!DOCTYPE[^>]*>', '', s)
    s = re.sub(r'<!--.*?-->', '', s, flags=re.S)  # Inkscape/editor comments precede the root element
    m = re.search(r'<svg[^>]*viewBox="([^"]+)"', s); vb = viewbox or m.group(1); vx, vy, vw, vh = map(float, vb.split())
    inner = re.sub(r'^\s*<svg[^>]*>', '', s.strip()); inner = re.sub(r'</svg>\s*$', '', inner)
    inner = re.sub(r'\.cls-', '.' + prefix + 'cls-', inner); inner = re.sub(r'class="cls-', 'class="' + prefix + 'cls-', inner)
    inner = re.sub(r'id="', 'id="' + prefix, inner); inner = re.sub(r'url\(#', 'url(#' + prefix, inner); inner = re.sub(r'xlink:href="#', 'xlink:href="#' + prefix, inner)
    w = h * vw / vh
    return f'<svg x="{x}" y="{y}" width="{w:.1f}" height="{h}" viewBox="{vb}">{inner}</svg>'
def windows(x, y, s):
    g = s * 0.08; q = (s - g) / 2
    return ''.join(f'<rect x="{x+i*(q+g):.1f}" y="{y+j*(q+g):.1f}" width="{q:.1f}" height="{q:.1f}" fill="#0078D4" rx="1"/>' for i in range(2) for j in range(2))
def powerbi(x, y, s):
    b = s * 0.26; gap = s * 0.11
    return (f'<rect x="{x:.1f}" y="{y+s*0.5:.1f}" width="{b:.1f}" height="{s*0.5:.1f}" rx="3" fill="#F2C811"/>'
            f'<rect x="{x+b+gap:.1f}" y="{y+s*0.25:.1f}" width="{b:.1f}" height="{s*0.75:.1f}" rx="3" fill="#F2C811"/>'
            f'<rect x="{x+2*(b+gap):.1f}" y="{y:.1f}" width="{b:.1f}" height="{s:.1f}" rx="3" fill="#E8B800"/>')
F = 'font-family="Helvetica Neue, Helvetica, Arial, sans-serif"'
svg = f'''<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" viewBox="0 0 1400 460" width="1400" height="460" {F}>
<defs>
  <marker id="arrow" viewBox="0 0 10 10" refX="8" refY="5" markerWidth="7" markerHeight="7" orient="auto-start-reverse"><path d="M0,0 L10,5 L0,10 z" fill="#333"/></marker>
  <filter id="shadow" x="-5%" y="-5%" width="110%" height="115%"><feDropShadow dx="0" dy="2" stdDeviation="3" flood-color="#000" flood-opacity="0.12"/></filter>
</defs>
<rect width="1400" height="460" fill="#ffffff"/>
<text x="700" y="44" text-anchor="middle" font-size="26" font-weight="700" fill="#222">Trino ODBC Driver — end-to-end data path</text>

<!-- OpenShift / Kubernetes cluster -->
<rect x="40" y="80" width="520" height="340" rx="18" fill="#fff4f4" stroke="#EE0000" stroke-width="2.5" stroke-dasharray="10 6"/>
{embed('openshift-logotype.svg', 70, 96, 46, 'os', viewbox='-1.7338671 -1.7338671 61.2633042 50.5')}
{embed('kubernetes-icon.svg', 134, 96, 46, 'k8')}
<text x="196" y="116" font-size="18" font-weight="700" fill="#222">OpenShift / Kubernetes cluster</text>
<text x="196" y="136" font-size="13" fill="#666">Trino deployed on OpenShift</text>

<rect x="80" y="215" width="440" height="150" rx="12" fill="#fff" stroke="#dd00a1" stroke-width="2" filter="url(#shadow)"/>
{embed('trino-logo.svg', 100, 231, 118, 'tr1')}
<text x="200" y="258" font-size="24" font-weight="700" fill="#222">Trino</text>
<text x="200" y="286" font-size="14" fill="#444">Distributed SQL query engine</text>
<text x="200" y="308" font-size="14" fill="#444">Coordinator + worker pods</text>
<text x="200" y="330" font-size="14" fill="#444">Catalogs: Hive, Iceberg, PostgreSQL, …</text>

<!-- Arrow: Trino -> Trino ODBC -->
<line x1="574" y1="290" x2="666" y2="290" stroke="#333" stroke-width="3" marker-start="url(#arrow)" marker-end="url(#arrow)"/>
<text x="616" y="249" text-anchor="middle" font-size="13" font-weight="600" fill="#222">HTTPS :443</text>
<text x="616" y="267" text-anchor="middle" font-size="12" fill="#555">Trino REST protocol</text>
<text x="616" y="313" text-anchor="middle" font-size="12" fill="#555">LDAP / AD · TLS</text>
<text x="616" y="329" text-anchor="middle" font-size="12" fill="#555">spooled segments</text>

<!-- Windows: Trino ODBC + Power BI -->
<rect x="680" y="80" width="680" height="340" rx="18" fill="#f3f8fd" stroke="#0078D4" stroke-width="2.5" stroke-dasharray="10 6"/>
{windows(706, 100, 36)}
<text x="756" y="112" font-size="20" font-weight="700" fill="#222">Windows</text>
<text x="756" y="134" font-size="14" fill="#666">ODBC Driver Manager (odbc32)</text>

<rect x="712" y="200" width="330" height="190" rx="12" fill="#fff" stroke="#dd00a1" stroke-width="2" filter="url(#shadow)"/>
{embed('trino-logo.svg', 728, 220, 110, 'tr3')}
<text x="818" y="248" font-size="24" font-weight="700" fill="#222">Trino ODBC</text>
<text x="818" y="278" font-size="14" fill="#444">Open-source ODBC 3.8 driver</text>
<text x="818" y="300" font-size="14" fill="#444">Windows · Linux · macOS</text>
<text x="818" y="322" font-size="14" fill="#444">LDAP/AD · TLS · fast fetch</text>
<text x="818" y="344" font-size="14" fill="#444">Unicode · block cursors · cancel</text>

<!-- Arrow: Trino ODBC -> Power BI -->
<line x1="1054" y1="295" x2="1146" y2="295" stroke="#333" stroke-width="3" marker-start="url(#arrow)" marker-end="url(#arrow)"/>
<text x="1096" y="256" text-anchor="middle" font-size="13" font-weight="600" fill="#222">ODBC API</text>
<text x="1096" y="274" text-anchor="middle" font-size="12" fill="#555">SQLFetchScroll</text>
<text x="1096" y="318" text-anchor="middle" font-size="12" fill="#555">SQLGetData</text>

<rect x="1158" y="200" width="172" height="190" rx="12" fill="#fffbe6" stroke="#E8B800" stroke-width="2" filter="url(#shadow)"/>
{powerbi(1214, 224, 60)}
<text x="1244" y="320" text-anchor="middle" font-size="22" font-weight="700" fill="#222">Power BI</text>
<text x="1244" y="344" text-anchor="middle" font-size="13" fill="#444">Desktop · Gateway</text>
<text x="1244" y="364" text-anchor="middle" font-size="13" fill="#444">Import / DirectQuery</text>
</svg>
'''
out = os.path.join(here, 'architecture.svg'); open(out, 'w').write(svg)
subprocess.run(['rsvg-convert', '-w', '2100', out, '-o', os.path.join(here, 'architecture.png')], check=True)
print('written', out)
