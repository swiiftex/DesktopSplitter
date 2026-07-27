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
