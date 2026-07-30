using System.Windows.Input;

namespace DesktopSplitter.ViewModels;

/// <summary>A numbered zone drawn on the editor canvas (FancyZones-style).</summary>
public sealed class ZoneVisual : ObservableObject
{
    private double _x, _y, _w, _h;
    private string _sizeText = string.Empty;
    private string _aspectNote = string.Empty;

    public ZoneVisual(int index) => Index = index;

    public int Index { get; }

    /// <summary>1-based number shown large in the middle of the zone.</summary>
    public int Number => Index + 1;

    public double X { get => _x; set => SetProperty(ref _x, value); }
    public double Y { get => _y; set => SetProperty(ref _y, value); }
    public double W { get => _w; set => SetProperty(ref _w, value); }
    public double H { get => _h; set => SetProperty(ref _h, value); }

    /// <summary>"2560x1440" — the segment's size in monitor pixels.</summary>
    public string SizeText { get => _sizeText; set => SetProperty(ref _sizeText, value); }

    /// <summary>Aspect-ratio badge such as "16:9", or "" when the zone matches no common ratio.</summary>
    public string AspectNote
    {
        get => _aspectNote;
        set
        {
            if (SetProperty(ref _aspectNote, value)) OnPropertyChanged(nameof(HasAspectNote));
        }
    }

    public bool HasAspectNote => !string.IsNullOrEmpty(_aspectNote);

    private bool _isSelected;
    private bool _isPrimary;
    private bool _isLaunchTarget;

    /// <summary>Drawn with an accent border while this zone's properties are being edited.</summary>
    public bool IsSelected
    {
        get => _isSelected;
        set => SetProperty(ref _isSelected, value);
    }

    public bool IsPrimary
    {
        get => _isPrimary;
        set { if (SetProperty(ref _isPrimary, value)) OnPropertyChanged(nameof(RoleBadge)); }
    }

    public bool IsLaunchTarget
    {
        get => _isLaunchTarget;
        set { if (SetProperty(ref _isLaunchTarget, value)) OnPropertyChanged(nameof(RoleBadge)); }
    }

    /// <summary>Short marker shown on the zone, e.g. "PRIMARY" or "NEW WINDOWS".</summary>
    public string RoleBadge => (IsPrimary, IsLaunchTarget) switch
    {
        (true, true) => "PRIMARY · NEW WINDOWS",
        (true, false) => "PRIMARY",
        (false, true) => "NEW WINDOWS",
        _ => string.Empty,
    };

    public bool HasRoleBadge => !string.IsNullOrEmpty(RoleBadge);

    /// <summary>Names the aspect ratio when the zone is within 1% of a well-known one.</summary>
    public static string DescribeAspect(int width, int height)
    {
        if (width <= 0 || height <= 0) return string.Empty;

        double actual = width / (double)height;
        (string Label, double Ratio)[] known =
        {
            ("16:9",  16.0 / 9.0),
            ("16:10", 16.0 / 10.0),
            ("21:9",  64.0 / 27.0),
            ("32:9",  32.0 / 9.0),
            ("4:3",   4.0 / 3.0),
        };

        foreach ((string label, double ratio) in known)
        {
            if (Math.Abs(actual - ratio) / ratio <= 0.01) return label;
        }
        return string.Empty;
    }
}

/// <summary>A draggable border between two adjacent zones.</summary>
public sealed class SplitterVisual : ObservableObject
{
    public const double Thickness = 7;

    private double _x, _y, _length;

    public SplitterVisual(int id, bool isVertical, int line)
    {
        Id = id;
        IsVertical = isVertical;
        Line = line;
    }

    /// <summary>Stable identity used by the view to route drag events back to the VM.</summary>
    public int Id { get; }

    /// <summary>True for a vertical bar that moves horizontally (resizes widths).</summary>
    public bool IsVertical { get; }

    /// <summary>Grid line index: the column (vertical) or row (horizontal) it sits after.</summary>
    public int Line { get; }

    public double X { get => _x; set => SetProperty(ref _x, value); }
    public double Y { get => _y; set => SetProperty(ref _y, value); }

    /// <summary>Extent along the bar's own axis (height for vertical, width for horizontal).</summary>
    public double Length
    {
        get => _length;
        set
        {
            if (!SetProperty(ref _length, value)) return;
            OnPropertyChanged(nameof(W));
            OnPropertyChanged(nameof(H));
        }
    }

    public double W => IsVertical ? Thickness : Length;
    public double H => IsVertical ? Length : Thickness;

    public Cursor DragCursor => IsVertical ? Cursors.SizeWE : Cursors.SizeNS;
}
