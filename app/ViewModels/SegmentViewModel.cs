using DesktopSplitter.Models;

namespace DesktopSplitter.ViewModels;

/// <summary>Editable resolution for a single layout segment.</summary>
public sealed class SegmentViewModel : ObservableObject
{
    private string _widthText = string.Empty;
    private string _heightText = string.Empty;
    private string _error = string.Empty;

    public SegmentViewModel(int index, int width, int height)
    {
        Index = index;
        _widthText = width.ToString();
        _heightText = height.ToString();
        Validate();
    }

    public int Index { get; }

    public string Title => $"Zone {Index + 1}";

    private bool _isSelected;
    private bool _isPrimary;
    private bool _showTaskbar = true;
    private bool _isLaunchTarget;

    /// <summary>Selected in the editor, so its properties are shown.</summary>
    public bool IsSelected
    {
        get => _isSelected;
        set => SetProperty(ref _isSelected, value);
    }

    /// <summary>Windows' primary display: taskbar, Start, Alt-Tab and the system tray live here.</summary>
    public bool IsPrimary
    {
        get => _isPrimary;
        set
        {
            if (!SetProperty(ref _isPrimary, value)) return;
            OnPropertyChanged(nameof(TaskbarLocked));
            OnPropertyChanged(nameof(EffectiveShowTaskbar));
            PrimaryRequested?.Invoke(this, EventArgs.Empty);
        }
    }

    /// <summary>Show Windows' taskbar on this zone.</summary>
    public bool ShowTaskbar
    {
        get => _showTaskbar;
        set
        {
            if (!SetProperty(ref _showTaskbar, value)) return;
            OnPropertyChanged(nameof(EffectiveShowTaskbar));
            RaiseChanged();
        }
    }

    /// <summary>New application windows open here.</summary>
    public bool IsLaunchTarget
    {
        get => _isLaunchTarget;
        set
        {
            if (!SetProperty(ref _isLaunchTarget, value)) return;
            if (value) LaunchTargetRequested?.Invoke(this, EventArgs.Empty);
        }
    }

    /// <summary>The primary always carries the main taskbar, so its checkbox is fixed on.</summary>
    public bool TaskbarLocked => IsPrimary;

    public bool EffectiveShowTaskbar => IsPrimary || ShowTaskbar;

    /// <summary>Raised when this zone asks to become primary (the parent enforces exclusivity).</summary>
    public event EventHandler? PrimaryRequested;

    /// <summary>Raised when this zone asks to become the launch target.</summary>
    public event EventHandler? LaunchTargetRequested;

    /// <summary>Sets the primary flag without re-raising the exclusivity event.</summary>
    public void SetPrimaryQuietly(bool value)
    {
        if (_isPrimary == value) return;
        _isPrimary = value;
        OnPropertyChanged(nameof(IsPrimary));
        OnPropertyChanged(nameof(TaskbarLocked));
        OnPropertyChanged(nameof(EffectiveShowTaskbar));
    }

    /// <summary>Sets the launch-target flag without re-raising the exclusivity event.</summary>
    public void SetLaunchTargetQuietly(bool value)
    {
        if (_isLaunchTarget == value) return;
        _isLaunchTarget = value;
        OnPropertyChanged(nameof(IsLaunchTarget));
    }

    public void SetShowTaskbarQuietly(bool value)
    {
        if (_showTaskbar == value) return;
        _showTaskbar = value;
        OnPropertyChanged(nameof(ShowTaskbar));
        OnPropertyChanged(nameof(EffectiveShowTaskbar));
    }

    public string WidthText
    {
        get => _widthText;
        set { if (SetProperty(ref _widthText, value)) { Validate(); RaiseChanged(); } }
    }

    public string HeightText
    {
        get => _heightText;
        set { if (SetProperty(ref _heightText, value)) { Validate(); RaiseChanged(); } }
    }

    public string Error
    {
        get => _error;
        private set { if (SetProperty(ref _error, value)) OnPropertyChanged(nameof(HasError)); }
    }

    public bool HasError => !string.IsNullOrEmpty(Error);

    public bool IsValid => !HasError;

    public int Width => int.TryParse(_widthText, out int w) ? w : 0;
    public int Height => int.TryParse(_heightText, out int h) ? h : 0;

    /// <summary>Raised whenever the edited value changes, so the parent can re-evaluate Apply.</summary>
    public event EventHandler? Changed;

    private void RaiseChanged() => Changed?.Invoke(this, EventArgs.Empty);

    public void Set(int width, int height)
    {
        _widthText = width.ToString();
        _heightText = height.ToString();
        OnPropertyChanged(nameof(WidthText));
        OnPropertyChanged(nameof(HeightText));
        Validate();
        RaiseChanged();
    }

    private void Validate()
    {
        if (!int.TryParse(_widthText, out int w) || !int.TryParse(_heightText, out int h))
        {
            Error = "Enter whole numbers.";
            return;
        }
        int lo = LayoutGeometry.MinSegmentDimension;
        int hi = LayoutGeometry.MaxSegmentDimension;
        if (w < lo || w > hi || h < lo || h > hi)
        {
            Error = $"Width and height must be {lo}..{hi}.";
            return;
        }
        Error = string.Empty;
    }
}
