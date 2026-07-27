namespace DesktopSplitter.Models;

/// <summary>Integer pixel rectangle (x, y, w, h).</summary>
public readonly record struct PixelRect(int X, int Y, int W, int H)
{
    public int Right => X + W;
    public int Bottom => Y + H;
    public override string ToString() => $"{W}x{H}+{X}+{Y}";
}
