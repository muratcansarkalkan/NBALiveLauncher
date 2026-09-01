Place the popup's TrueType font here as:

    scoreboard.ttf

Then set "fontFace" in ../popup.json to the font's internal family name.
The family name is often different from the filename. For example, a file
named espn-scoreboard-bold.ttf may have the family name "ESPN Scoreboard".

If the file is missing or the family name does not match, the renderer uses
the Windows fallback selected by fontFace (Arial in the sample config).
