param([string]$Output = (Join-Path $PSScriptRoot '..\assets\app.ico'))
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
# A code-native brand mark; no font or image assets are bundled.
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;
using System.IO;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
public static class TimeTraceIcon {
    public static void Write(string path) {
        int[] sizes = { 16, 24, 32, 48, 64, 128, 256 };
        var frames = new List<byte[]>();
        foreach (int size in sizes) {
            using (var image = new Bitmap(size, size, PixelFormat.Format32bppArgb)) {
                using (var g = Graphics.FromImage(image)) {
                    g.SmoothingMode = SmoothingMode.AntiAlias;
                    g.Clear(Color.Transparent);
                    g.ScaleTransform(size / 64f, size / 64f);
                    using (var shape = new GraphicsPath()) {
                        shape.AddArc(2, 2, 28, 28, 180, 90);
                        shape.AddArc(34, 2, 28, 28, 270, 90);
                        shape.AddArc(34, 34, 28, 28, 0, 90);
                        shape.AddArc(2, 34, 28, 28, 90, 90);
                        shape.CloseFigure();
                        using (var blue = new SolidBrush(Color.FromArgb(0, 122, 255)))
                            g.FillPath(blue, shape);
                    }
                    using (var pen = new Pen(Color.White, 3)) {
                        pen.StartCap = pen.EndCap = LineCap.Round;
                        g.DrawEllipse(pen, 13, 13, 38, 38);
                        g.DrawLines(pen, new PointF[] { new PointF(32, 20), new PointF(32, 32), new PointF(42, 38) });
                    }
                }
                using (var png = new MemoryStream()) {
                    image.Save(png, ImageFormat.Png);
                    frames.Add(png.ToArray());
                }
            }
        }
        using (var writer = new BinaryWriter(File.Create(path))) {
            writer.Write((ushort)0); writer.Write((ushort)1); writer.Write((ushort)sizes.Length);
            uint offset = (uint)(6 + 16 * sizes.Length);
            for (int i = 0; i < sizes.Length; i++) {
                writer.Write((byte)(sizes[i] == 256 ? 0 : sizes[i]));
                writer.Write((byte)(sizes[i] == 256 ? 0 : sizes[i]));
                writer.Write((byte)0); writer.Write((byte)0);
                writer.Write((ushort)1); writer.Write((ushort)32);
                writer.Write((uint)frames[i].Length); writer.Write(offset);
                offset += (uint)frames[i].Length;
            }
            foreach (var frame in frames) writer.Write(frame);
        }
    }
}
'@
[TimeTraceIcon]::Write([System.IO.Path]::GetFullPath($Output))
