# Flux Core Documentation

Flux-core documentation currently consists of man pages written in ReStructured Text (rst). The man pages are generated using sphinx and are also hosted at flux-framework.readthedocs.io.

##  Build Instructions

To build with python virtual environments:

```bash
virtualenv -p python3 sphinx-rtd
source sphinx-rtd/bin/activate
git clone git@github.com:flux-framework/flux-core
cd flux-core/doc
pip install -r requirements.txt
make man
```

## Adding a New Man Page

Generating a man pages is done via the `man_pages` variable in `conf.py`. For example:

```
man_pages = [
    ('man1/flux', 'flux', 'the Flux resource management framework', [author], 1),
]
```

The tuple entry in the `man_pages` list specifies:
- File name (relative path, without the `.rst` extension)
- Name of man page
- Description of man page
- Author (use `[author]` as in the example)
- Manual section for the generated man page
