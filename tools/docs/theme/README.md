# Docs theme

Three stylesheets and one header, applied by `tools/docs/Doxyfile` and
`Doxyfile.package`:

| file | what it is |
| --- | --- |
| `doxygen-awesome.css`, `doxygen-awesome-sidebar-only.css` | upstream [doxygen-awesome-css](https://github.com/jothepro/doxygen-awesome-css) 2.3, MIT (`LICENSE`), unmodified |
| `deki.css` | the Deki theme: dekiengine.com's palette, fonts and square corners, the site bar, and the layout offsets that make room for it |
| `header.html` | Doxygen's header template plus the webfonts and the site bar |

Both Doxyfiles set `HTML_COLORSTYLE = DARK`. doxygen-awesome keeps its dark
values on `html.dark-mode`, which outranks a bare `html`, so `deki.css`
declares every variable for both selectors.

## Changing the header

Regenerate the base template when Doxygen is upgraded, then re-apply the two
additions (the `<link>`s for the fonts, and the `div.deki-bar` after `<body>`):

    doxygen -w html header.html footer.html stylesheet.css

The header is a template of ours too: `build_docs.py` writes one copy per
language (`localize.header`), which points the bar at that language's pages
on dekiengine.com, puts the language's Noto Sans where `<!--DEKI_FONTS-->`
is and the language menu where `<!--DEKI_LANGUAGES-->` is. Keep both
markers, and keep the bar's links and words as they are, since the
translation replaces them by their exact text.

**Do not write `<`+`!--BEGIN X--`+`>` or its END twin anywhere in the file,
including inside a comment.** They are the template language, and Doxygen
scans the whole file for them: a marker named inside a comment opened a
section that was never closed, and the rest of that comment rendered as text
at the top of every page.

Whatever comment the header carries is copied into every generated page, so
keep it short and keep internal detail here instead.

## The Discord link

The bar links to `https://dekiengine.com/discord`, not to a `discord.gg`
invite. The invite is written once, in `hugo-dekiengine/hugo.toml`, and that
short link is what lets it be rotated in one place instead of in every README,
page and site that ever quoted it.

## Checking a change

`build_docs.py --reuse-metadata` skips the editor run and rebuilds the HTML
from the metadata already in `build/docs/`, which takes seconds rather than
minutes. Add `--no-packages` to build only the main site. Nothing serves the
result, so open `build/docs/html/index.html` or point a static server at it -
but not with that directory as its working directory, or the next build cannot
delete it on Windows.
