#!/bin/bash
# Copies the library app and the inbox API to crosspoint.zenix.vn:
#   app/    -> /usr/local/lsws/crosspoint.zenix.vn/library-app   (outside the web root)
#   public/ -> /usr/local/lsws/crosspoint.zenix.vn/html/library
# and installs the indexer cron job. Run from this folder.
set -e
cd "$(dirname "$0")"
HOST=zenix-server
PORT=8430
SITE=/usr/local/lsws/crosspoint.zenix.vn
tar -cf - app public | ssh -p $PORT $HOST "set -e
  mkdir -p $SITE/library-app $SITE/html/library $SITE/library-data
  tar -xf - -C /tmp && cp -r /tmp/app/. $SITE/library-app/ && cp -r /tmp/public/. $SITE/html/library/ && rm -rf /tmp/app /tmp/public
  chown -R nobody:nobody $SITE/library-app $SITE/html/library $SITE/library-data
  # Like the server's other jobs: one file in /etc/cron.d, run as the web user.
  printf '%s\n' \
    '*/10 * * * * nobody /usr/local/bin/php $SITE/library-app/indexer.php >> $SITE/library-data/indexer.log 2>&1' \
    '5,25,45 * * * * nobody /usr/local/bin/php $SITE/library-app/fetcher.php >> $SITE/library-data/fetcher.log 2>&1' \
    '50 * * * * nobody /usr/local/bin/php $SITE/library-app/rights.php >> $SITE/library-data/rights.log 2>&1' \
    > /etc/cron.d/crosspoint-library
  chmod 644 /etc/cron.d/crosspoint-library"
# The book inbox API (devices register and collect through it).
scp -q -P $PORT ../inbox/api.php ../inbox/index.html $HOST:/tmp/ && ssh -p $PORT $HOST "set -e
  cp /tmp/api.php /tmp/index.html $SITE/html/inbox/ && rm -f /tmp/api.php /tmp/index.html
  chown nobody:nobody $SITE/html/inbox/api.php $SITE/html/inbox/index.html"
echo deployed
