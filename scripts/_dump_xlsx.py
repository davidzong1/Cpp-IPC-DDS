"""Dump performance.xlsx content (sheets, cells, charts) as plain text."""
import zipfile
import re

path = "/home/zwc/cpp_ipc_dds/performance.xlsx"
z = zipfile.ZipFile(path)
names = z.namelist()
print("=== PARTS ===")
for n in names:
    print(n)

shared = []
if "xl/sharedStrings.xml" in names:
    xml = z.read("xl/sharedStrings.xml").decode("utf-8", "replace")
    for si in re.findall(r"<si>(.*?)</si>", xml, re.S):
        txt = "".join(re.findall(r"<t[^>]*>(.*?)</t>", si, re.S))
        txt = (txt.replace("&amp;", "&").replace("&lt;", "<")
                  .replace("&gt;", ">").replace("&quot;", '"'))
        shared.append(txt)
print("\n=== SHARED STRINGS: %d ===" % len(shared))

if "xl/workbook.xml" in names:
    wb = z.read("xl/workbook.xml").decode("utf-8", "replace")
    print("\n=== SHEETS ===")
    for m in re.finditer(r'<sheet[^>]*name="([^"]*)"', wb):
        print(m.group(1))


def col_of(ref):
    return re.match(r"([A-Z]+)", ref).group(1)


for n in sorted(nm for nm in names if re.match(r"xl/worksheets/sheet\d+\.xml$", nm)):
    xml = z.read(n).decode("utf-8", "replace")
    print("\n\n########## %s ##########" % n)
    rows = re.findall(r"<row[^>]*r=\"(\d+)\"[^>]*>(.*?)</row>", xml, re.S)
    for rnum, rowxml in rows:
        cells = []
        for c in re.finditer(r"<c\s+([^>]*?)>(.*?)</c>|<c\s+([^>]*?)/>", rowxml, re.S):
            attrs = c.group(1) or c.group(3) or ""
            body = c.group(2) or ""
            ref = re.search(r'r="([A-Z]+\d+)"', attrs)
            ref = ref.group(1) if ref else "?1"
            t = re.search(r't="([^"]*)"', attrs)
            t = t.group(1) if t else "n"
            v = re.search(r"<v>(.*?)</v>", body, re.S)
            val = v.group(1) if v else ""
            if t == "s" and val.isdigit():
                val = shared[int(val)]
            elif t == "inlineStr":
                val = "".join(re.findall(r"<t[^>]*>(.*?)</t>", body, re.S))
            if val != "":
                cells.append("%s=%s" % (col_of(ref), val))
        if cells:
            print("R%s | %s" % (rnum, " | ".join(cells)))

print("\n\n=== CHARTS ===")
for n in names:
    if "chart" in n.lower() and n.endswith(".xml"):
        print("---", n)
        xml = z.read(n).decode("utf-8", "replace")
        print("   texts:", re.findall(r"<a:t>(.*?)</a:t>", xml, re.S)[:40])
        print("   types:", set(re.findall(r"<c:(\w+Chart)>", xml)))
        print("   refs:", re.findall(r"<c:f>(.*?)</c:f>", xml)[:30])
