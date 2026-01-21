# yeyito-browser
This is my custom fork of qutebrowser with extended functionality, feel free to use it.
This README.md only details the added functionality of this browser in comparison to qutebrowser.
For information about literally anything else, refer to the qutebrowser's readme that you can find [here](https://github.com/qutebrowser/qutebrowser)
I expect you to build this from source. This can get rather tedious so I've made an install script for myself: install.sh. I've included it in this repo so you can modify it to your needs.

## Added features:
- Run arbitrary javascript through hints by doing this:
```py
config.bind('<Ctrl-Space>', 'hint scrollables javascript focus.js') # focus.js goes in js/ dir in the same dir as config.py
```
- Configure per-site cookies like so:
```py
c.content.cookies.accept = 'no-3rdparty'
c.content.cookies.thirdparty_whitelist = [
    "*://*.recaptcha.net/*",
    "*://*.hcaptcha.com/*",
    "*://accounts.google.com/*",
]
```
- Separate file for permission management that you can include in your config.py:
```py
config.source('permissions.py')
```

- Default hints with f no longer select elements that are less than 3px wide, have an invisible parent, or have 0 opacity:

- Added hand-tuned rightclickables, hoverables, and scrollables hinting selectors, I use them like so:
```py
config.bind('<Ctrl-Space>', 'hint scrollables focus')
config.bind('<Ctrl-J>', 'hint rightclickables right-click')
config.bind('<Ctrl-K>', 'hint hoverables hover')
```

- Greatly improved css overrides, you can now use globbing. I use it like so:
```py
config.set("content.user_stylesheets", ["~/.config/qutebrowser/cssoverrides/default.css"])
config.set("content.user_stylesheets", ["~/.config/qutebrowser/cssoverrides/pdf.css"], "qute://pdfjs/web/viewer.html?filename=*")
config.set("content.user_stylesheets", ["~/.config/qutebrowser/cssoverrides/github.css"], "github.com/*")
config.set("content.user_stylesheets", ["~/.config/qutebrowser/cssoverrides/polymarket.css"], "polymarket.com/*")
config.set("content.user_stylesheets", ["~/.config/qutebrowser/cssoverrides/null.css"], "monkeytype.com/*")
config.set("content.user_stylesheets", ["~/.config/qutebrowser/cssoverrides/null.css"], "excalidraw.com/*")
config.set("content.user_stylesheets", ["~/.config/qutebrowser/cssoverrides/null.css"], "localhost:*/*")
config.set("content.user_stylesheets", ["~/.config/qutebrowser/cssoverrides/null.css"], "127.0.0.1:*/*")
```

- I've added other things but honestly I forget.

## Final note
I'm making the perfect browser for ME, prs and issues are greatly appreciated though. However if you encounter a problem, a missing feature or something that YOU would like specifically, honestly, just fork it and add it/fix it yourself! This whole project started because I was annoyed that there was no sane way to run javascript after a hint in qutebrowser anyway.
