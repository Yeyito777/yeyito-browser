# Bugs
- Opening a pdf, quitting and coming back creates a fake tab? (I think this is a symptom of a bigger issue, if I :wq qutebrowser it can save a bunch of fake tabs in place of the ones that it wasn't able to load while it was starting up)
- Default "All" selector accessed by pressing f to enter click hint mode doesn't highlight all elements that I'd like to click (click events, checkboxes, etc...)
- Fix "Cannot read properties of undefined reading (length)" when selecting a text input
- Make ai point to claude opus and remove gem. (You might just buy the claude 100$ subscription lol)
- The greasemonkey script that controls autosending on ai is overridden by any saved messages on the text input (chatgpt), this is irrelevant if we fully switch to claude opus.
- "Unknown error while getting elements" Is so fucking annoying when I press f
- The fake click that is sent on hinting may not click on the desired item
- Hovering over an element with Ctrl+k then pressing f sometimes unhovers? (I think conditions have to be just right such that I'm bareeely hovering over it?)
- Why is the discord app so fucking slow, I think qutebrowser as a whole needs to be made faster / slowness needs to be diagnosed
- Prompted again on javascript yes/Always/no/Never promps if selecting Always or Never?
- Hinting must highlight elements if ANY part of their bounding box intersects with their screen, not just the top-left corner as I suspect it currently does.

# Improvements
- Add fine-grained selectors. f should highlight all elements with click events, right click all with right click events, and hover all with hover events. + extras for all those
- Add a way to click outside of images once you've focused them
- Allow for website-scoped css aplication so I can go crazy with the neo-blue theme and start improving it per-website like darkreader.
- zz centers selection on caret mode and highlight mode (/ and ?)
- Overhaul UIs like the crash report and the download so that they follow my terminal theme (Also make it so that it doesn't email the dude lol)
- Use TamperMonkey instead of GreaseMonkey or something that would help get vencord in discord
- A 'copy' hinting mode that allows for copying big, but independent blocks of text
