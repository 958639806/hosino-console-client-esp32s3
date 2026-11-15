param(
    [string]$SourcePath = (Join-Path $PSScriptRoot '..\overlay\main\hosino_corner_clip.c')
)
$ErrorActionPreference = 'Stop'
# Windows has a C# compiler via Add-Type, while the installed ESP toolchain
# only produces Xtensa binaries. Compile the actual pure integer C function
# bodies after type/access spelling changes; do not reimplement the algorithm.
$source = Get-Content -LiteralPath $SourcePath -Raw
$match = [regex]::Match($source, '(?s)/\* CORNER_GEOMETRY_BEGIN.*?\*/(.*?)/\* CORNER_GEOMETRY_END \*/')
if (-not $match.Success) { throw 'Geometry block not found' }
$geometry = $match.Groups[1].Value
$geometry = $geometry -replace '\buint32_t\b', 'uint' -replace '\buint16_t\b', 'ushort'
$geometry = $geometry -replace '(?m)^static (uint|int|ushort|hosino_corner_clip_box_t) ', 'public static $1 '
$geometry = $geometry -replace '(?m)^(int|ushort|hosino_corner_clip_box_t) (hosino_\w+\()', 'public static $1 $2'
$geometry = $geometry -replace 'hosino_corner_clip_box_t result = \{0\};', 'hosino_corner_clip_box_t result = new hosino_corner_clip_box_t();'
$harness = @'
using System;
public struct hosino_corner_clip_box_t {
    public int x1, y1, x2, y2;
    public bool visible;
}
public static class CornerGeometryTest {
__GEOMETRY__
    static long checks;
    static void Check(bool ok, string name) {
        checks++;
        if (!ok) throw new Exception(name);
    }
    static void CheckBox(int x1, int y1, int x2, int y2, ushort radius) {
        var box = hosino_corner_clip_bounds(x1, y1, x2, y2, radius);
        if (box.visible) {
            Check(box.x1 >= x1 && box.y1 >= y1 && box.x2 <= x2 && box.y2 <= y2,
                  "box exceeds source");
            Check(box.x1 < box.x2 && box.y1 < box.y2, "empty visible box");
            Check(((box.x1 | box.y1 | box.x2 | box.y2) & 1) == 0, "odd alignment");
            Check((box.x2-box.x1)*(box.y2-box.y1) <= (x2-x1)*(y2-y1), "scratch exceeds input");
        }
        bool any = false;
        // Each row's retained pixels are contiguous: testing both endpoints
        // proves the box contains every unmasked pixel on that row.
        for (int y=y1; y<y2; y++) {
            int inset = hosino_corner_clip_row_inset(y, radius);
            int left = Math.Max(x1, inset), right = Math.Min(x2, 480-inset);
            if (left >= right) continue;
            any = true;
            Check(box.visible && box.x1 <= left && box.x2 >= right &&
                  box.y1 <= y && box.y2 > y, "retained pixel omitted");
        }
        Check(box.visible == any, "incorrect empty-region decision");
        if (radius == 0) Check(box.visible && box.x1 == x1 && box.y1 == y1 &&
                             box.x2 == x2 && box.y2 == y2, "radius zero changes region");
    }
    public static string Run() {
        Random random = new Random(1616);
        for (ushort r=0; r<=240; r++) {
            for (int y=0; y<480; y++) {
                int inset = hosino_corner_clip_row_inset(y, r);
                Check(inset >= 0 && inset <= r && (inset&1)==0, "invalid inset");
                Check(inset == hosino_corner_clip_row_inset(479-y,r), "asymmetric rows");
                if (y<239) Check(inset >= hosino_corner_clip_row_inset(y+1,r), "nonmonotonic corner");
                // Independent circle oracle: no removed pixel box may touch
                // the ideal quarter circle, including at its nearest edge.
                int dy = Math.Max(0, r-Math.Min(y,479-y)-1);
                for (int x=0; x<inset; x++) {
                    int dx = Math.Max(0,r-x-1);
                    Check(dx*dx+dy*dy > r*r, "circle-intersecting pixel clipped");
                }
            }
            CheckBox(0,0,480,480,r);
            for (int y=0; y<480; y+=2) {
                CheckBox(0,y,480,Math.Min(480,y+50),r);
                CheckBox(0,y,2,y+2,r);
                CheckBox(478,y,480,y+2,r);
            }
            for (int i=0; i<256; i++) {
                int x1=2*random.Next(240), y1=2*random.Next(240);
                int x2=2*random.Next(x1/2+1,241), y2=2*random.Next(y1/2+1,241);
                CheckBox(x1,y1,x2,y2,r);
            }
        }
        Check(hosino_corner_clip_row_inset(-1,100)==0, "negative row");
        Check(hosino_corner_clip_row_inset(480,100)==0, "outside row");
        Check(hosino_corner_clip_row_inset(0,-1)==0, "negative radius");
        Check(hosino_corner_clip_row_inset(0,65535)==hosino_corner_clip_row_inset(0,240), "radius clamp");
        Check(!hosino_corner_clip_bounds(1,0,480,480,240).visible, "unaligned input accepted");
        Check(!hosino_corner_clip_bounds(0,0,482,480,240).visible, "out of range accepted");
        Check(!hosino_corner_clip_bounds(0,0,0,2,240).visible, "empty input accepted");
        return "PASS: all 241 radii; geometry/bounds/alignment/independent circle oracle; " + checks + " assertions";
    }
}
'@
Add-Type -TypeDefinition ($harness.Replace('__GEOMETRY__', $geometry)) -Language CSharp
[CornerGeometryTest]::Run()
