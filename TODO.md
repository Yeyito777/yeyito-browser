# Bugs
- Opening a pdf, quitting and coming back creates a fake tab? (I think this is a symptom of a bigger issue, if I :wq qutebrowser it can save a bunch of fake tabs in place of the ones that it wasn't able to load while it was starting up)
- If I'm in caret mode and don't select anything then I need to use hinting or click on the page so we go back into normal mode // page has focus (it freezes on a mode?)
- Default "All" selector accessed by pressing f to enter click hint mode doesn't highlight all elements that I'd like to click (click events, checkboxes, etc...)
- Get rid of fatal crash screen I don't wanna report shit.
- Fix "Cannot read properties of undefined reading (length)" when selecting a text input
- AI sometimes doesn't fire
- "Unknown error while getting elements" Is so fucking annoying when I press f
- The fake click that is sent on hinting may not click on the desired item
- Check if css styles are being applied twice. (double-mapping) a way to fix this would be to allow an exclude: List[str] option which excludes other themes from being loaded onto a page by other themes?
- Hovering over an element with Ctrl+k then pressing f sometimes unhovers? (I think conditions have to be just right such that I'm bareeely hovering over it?)

# Improvements
- Add fine-grained selectors. f should highlight all elements with click events, right click all with right click events, and hover all with hover events. + extras for all those
- Add a way to click outside of images once you've focused them
- Allow for website-scoped css aplication so I can go crazy with the neo-blue theme and start improving it per-website like darkreader.
- zz centers selection on caret mode and highlight mode (/ and ?)
- Overhaul UIs like the crash report and the download so that they follow my terminal theme
