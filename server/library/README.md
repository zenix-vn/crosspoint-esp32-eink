# CrossPoint library (crosspoint.zenix.vn/library)

A web library for the e-book collection and the vocabulary decks, open to
anyone who registers, usable from a browser and from the X4 Pro.

- **Accounts**: anyone signs up with an email and password, or with Google
  (when `library-data/google.json` is present). There is no approval step; the
  admin's **Quản lý** page lists the accounts and the history of what they do
  (sign-ups, linked readers, files sent and received), can disable or delete
  an account, and switches registration and access to the shared collection.
  The first release's single login (`auth.json`) became the admin account.
- **Readers**: a user makes a link key on **Thiết bị** and types it on the
  reader (*More → Receive books*, or the web settings of older firmware). The
  reader then joins that account; books and word lists the user sends wait in
  the reader's own inbox until it collects them. Each user sees and sends to
  their own readers only.
- **Books**: browse the shared collection by category and group, search
  without diacritics, download, or send an EPUB to a reader. The admin also
  marks favourites and reading status and edits title, author, category and tags.
- **My files**: each user uploads their own books and `.tsv` word lists
  (500 MB per account) and sends them to their readers.
- **Vocabulary**: look up words, browse, download and send the published decks,
  the admin's decks made from books, and the user's own. For an English EPUB,
  the admin's *Create word list* builds a deck of the book's less common words.
- **OPDS** (`/library/opds.php`, HTTP Basic with the account's email and
  password): the X4 Pro's OPDS browser lists categories, new books and search,
  EPUB and TXT only.

### Google sign-in

Create an OAuth client (type *Web application*) in Google Cloud Console with
the redirect URI `https://crosspoint.zenix.vn/library/index.php?p=google`, then
put `{"client_id": "...", "client_secret": "..."}` in
`library-data/google.json` (owner `nobody`, mode 640). The Google button shows
up as soon as the file exists.

## Layout on the server

| Path | Contents |
| --- | --- |
| `html/library/` | the pages (`public/` here) |
| `library-app/` | shared code, indexer, setup (`app/` here), outside the web root |
| `library-data/books/` | the collection, uploaded as is |
| `library-data/library.sqlite` | books, accounts, readers, the events log, dictionary |
| `library-data/covers/`, `decks/` | cover thumbnails, decks made from books |
| `library-data/users/<id>/` | each account's uploaded books and decks |
| `inbox-data/devices/<mac>/` | what waits for each reader |

Everything under `library-data` is outside the web root; files are served only
to a logged-in browser or Basic credentials.

## Operation

```
bash deploy.sh                                                   # copy app/ and public/
ssh zenix-server 'sudo -u nobody php .../library-app/setup.php toanlb [password]'  # set the admin password
ssh zenix-server 'sudo -u nobody php .../library-app/indexer.php'                  # pick up new files
```

The indexer adds new files (metadata and cover from the EPUB, otherwise from
the file name "Title - Author"), drops deleted ones and keeps edits made on the
web. `deploy.sh` installs it as a 10-minute cron job. New books can be copied
into `library-data/books/<category>/<group>/` with any tool.

The fetcher (`fetcher.php`, every 20 minutes) adds freely licensed EPUBs a few
at a time: Project Gutenberg's 1,000 most downloaded English books into
`z.Sách tiếng Anh/<shelf>`, and Vietnamese Wikisource prose (novels, short
stories, reportage) into `Văn Học/Việt Nam/Wikisource`. It skips titles the
library already has and keeps its progress in `library-data/fetch/state.json`;
the log is `library-data/fetcher.log`.

The copyright review (`rights.php`, hourly) checks every book not reviewed
yet. Gutenberg and Wikisource editions are free. Any other book is free only
when every author died in 1975 or earlier (Vietnam: 50 years after death,
looked up on Wikidata) and the book is in the author's own language, since a
translation is also the translator's. Everything else is flagged **Bản
quyền**: hidden from every account but the admin's and from OPDS. The admin
sees a red label, the *Bản quyền* filter on the book list, and on the book
page can delete it or mark it free. `php rights.php --dry 40` prints the
verdict for 40 random unreviewed books without changing them.
