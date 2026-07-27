# Storm tracking plan

## Goal

Add a lightweight storm-monitoring feature to the firmware so the device can watch for thunderstorm activity, track whether a storm is approaching, and report when it is close enough to matter.

## Current direction

The firmware already has:
- Wi-Fi connectivity
- HTTP client support
- a console REPL
- a display/UI path

That makes it practical to add a simple polling-based storm watcher without introducing a large new subsystem.

## Proposed behavior

1. When Wi-Fi is up, the board polls a weather source periodically.
2. The board classifies the current situation into broad states such as:
   - no signal
   - storm watch
   - thunderstorm chance rising
   - close/active storm
3. The status is shown on the console via a dedicated command.
4. In a later step, the same information can drive a UI indicator or a more visible alert.

## Near-term implementation steps

1. Add a weather monitoring module that can:
   - fetch data over HTTP
   - parse the response for storm-related signals
   - maintain a simple internal report structure
2. Expose the state through a console command such as:
   - weather_status
3. Start the monitor after Wi-Fi is active so it only runs when the network path is usable.
4. Keep the logic conservative and simple at first, with room to refine later.

## Constraints

- Prefer a small, maintainable implementation.
- Avoid adding unnecessary memory or stack usage.
- Keep the console output easy to read.
- Do not assume the board will have a perfect weather feed; treat any remote fetch as best-effort.

## Future enhancements

After the basic monitor works, the next improvements could be:
- richer state transitions for “approaching” vs “nearby”
- UI updates on the glass
- more frequent polling when conditions look concerning
- support for local radar or lightning data if a better source becomes available

## Success criteria

The feature is considered complete when:
- the board can poll a weather source while connected to Wi-Fi
- the console can report a clear storm status
- the implementation is small enough to fit comfortably in the existing firmware structure
