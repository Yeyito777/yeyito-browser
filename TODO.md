# Bugs
- "Unknown error while getting elements" Is so fucking annoying when I press f
- The fake click that is sent on hinting may not click on the desired item
- Hinting must highlight elements if ANY part of their bounding box intersects with their screen, not just the top-left corner as I suspect it currently does.
- Fix the fact that when I rebuild qutebrowser oftentimes credentials are lost and I need to relog into discord for example.
- Stop CI in github please
- Body / app body in pages like youtube do not get shown through selectables so if we're scrolling in another element it's a pain to refocus the main scroll.
- Scrollbars are not toggled with the shader toggle enum
- You need to fix everything in the shdaer stress test

# Improvements
- zz centers selection on caret mode and highlight mode (/ and ?)
- Overhaul UIs like the crash report and the download so that they follow my terminal theme (Also make it so that it doesn't email the dude lol)
- Use TamperMonkey instead of GreaseMonkey or something that would help get vencord in discord
- A 'copy' hinting mode that allows for copying big, but independent blocks of text
- Be able to navigate the right click copy menu etc with keybinds / outright overhaul it

# Major aditions
- Really think how I could add $/0/w/W/f/F and others while writing text in insert mode. Maybe a special mode? Like insert-normal mode?
