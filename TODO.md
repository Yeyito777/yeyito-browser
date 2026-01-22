# Bugs
- Opening a pdf, quitting and coming back creates a fake tab? (I think this is a symptom of a bigger issue, if I :wq qutebrowser it can save a bunch of fake tabs in place of the ones that it wasn't able to load while it was starting up)
- The greasemonkey script that controls autosending on ai is overridden by any saved messages on the text input (chatgpt), this is irrelevant if we fully switch to claude opus.
- "Unknown error while getting elements" Is so fucking annoying when I press f
- The fake click that is sent on hinting may not click on the desired item
- Hovering over an element with Ctrl+k then pressing f sometimes unhovers? (I think conditions have to be just right such that I'm bareeely hovering over it?)
- Why is the discord app so fucking slow, I think qutebrowser as a whole needs to be made faster / slowness needs to be diagnosed
- Prompted again on javascript yes/Always/no/Never promps if selecting Always or Never?
- Hinting must highlight elements if ANY part of their bounding box intersects with their screen, not just the top-left corner as I suspect it currently does.
- Devtools must be col_bg and have the accents
- Fix the fact that when I rebuild qutebrowser oftentimes credentials are lost and I need to relog into discord for example.
- Stop CI in github please
- Body / app body in pages like youtube do not get shown through selectables so if we're scrolling in another element it's a pain to refocus the main scroll.

# Improvements
- Add a way to click outside of images once you've focused them
- zz centers selection on caret mode and highlight mode (/ and ?)
- Overhaul UIs like the crash report and the download so that they follow my terminal theme (Also make it so that it doesn't email the dude lol)
- Use TamperMonkey instead of GreaseMonkey or something that would help get vencord in discord
- A 'copy' hinting mode that allows for copying big, but independent blocks of text
- Be able to navigate the right click copy menu etc with keybinds / outright overhaul it

# Major aditions
- Really think how I could add $/0/w/W/f/F and others while writing text in insert mode. Maybe a special mode? Like insert-normal mode?
