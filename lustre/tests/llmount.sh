#!/bin/sh
# suggested boilerplate for test script

LCONF=${LCONF:-../utils/lconf}
NAME=${NAME:-local}

config=$NAME.xml
mkconfig=$NAME.sh

if [ "$PORTALS" ]; then
  portals_opt="--portals=$PORTALS"
fi

if [ "$LUSTRE" ]; then
  lustre_opt="--lustre=$LUSTRE"
fi

if [ "$1" = "-v" ]; then
  verbose="-v"
fi

[ -x $LCONF ] || chmod a+rx $LCONF

sh $mkconfig $config || exit 1

${LCONF} $portals_opt $lustre_opt --reformat --gdb $verbose $config  || exit 2
