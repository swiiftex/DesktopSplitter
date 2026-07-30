using System.Threading;
using DesktopSplitter.Services;

namespace DesktopSplitter.ViewModels;

/// <summary>
/// Bridges <see cref="ApplyService"/> (background thread) to the "Keep these display settings?"
/// countdown on the UI thread.
///
/// The window's own timer only animates the number — THIS wait is the authority, and it defaults
/// to revert. That ordering matters: if the change left the user unable to see or click the
/// dialog, doing nothing must undo it.
/// </summary>
public sealed class ApplyConfirmer : IApplyConfirmer
{
    private readonly IHidingUi _ui;
    private readonly Action<string> _log;

    public ApplyConfirmer(IHidingUi ui, Action<string> log)
    {
        _ui = ui;
        _log = log;
    }

    public bool ConfirmApply(string summary, int timeoutSeconds, CancellationToken ct)
    {
        System.Windows.Threading.Dispatcher? dispatcher = System.Windows.Application.Current?.Dispatcher;
        if (dispatcher is null) return true;   // headless: nothing to confirm against

        using var answered = new ManualResetEventSlim(false);
        int keep = 0;
        ICountdownHandle? handle = null;

        dispatcher.Invoke(() =>
        {
            handle = _ui.ShowApplyCountdown(
                summary, timeoutSeconds,
                onKeep: () => { Interlocked.Exchange(ref keep, 1); answered.Set(); },
                onRevert: () => { Interlocked.Exchange(ref keep, 0); answered.Set(); });
        });

        bool gotAnswer;
        try
        {
            gotAnswer = answered.Wait(TimeSpan.FromSeconds(timeoutSeconds + 1), ct);
        }
        catch (OperationCanceledException)
        {
            dispatcher.Invoke(() => handle?.CloseFromHelper());
            return false;
        }

        dispatcher.Invoke(() => handle?.CloseFromHelper());

        if (!gotAnswer)
        {
            _log($"No answer within {timeoutSeconds}s — reverting the change automatically.");
            return false;
        }
        return Volatile.Read(ref keep) == 1;
    }
}
