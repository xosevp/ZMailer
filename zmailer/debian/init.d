#! /bin/sh
#
# ZMailer startup and maintenance commands
#
#	Copyright 1990 by Rayan S. Zachariassen, all rights reserved.
#	This will be free software, but only when it is finished.
#
#	Some hacking by Matti Aarnio, Copyright 1990-1997
#	Modified for Debian by Christoph Lameter
#
#	Some other modifications by Remco vd Meent <remco@debian.org>
#
# Comment this line out when finished configuring:
exit 0;
# 

test -e /etc/zmailer/zmailer.conf -a -e /var/spool/postoffice || exit 0
test -e /usr/lib/zmailer -a -e /usr/lib/zmailer/zmailer || exit 0

PATH=/bin:/usr/bin:/sbin:/usr/sbin
#if [ -d /usr/ucb ]; then
#  PATH=$PATH:/usr/ucb
#fi
FLAGS="defaults 50"
ZCONFIG=/etc/zmailer/zmailer.conf

# Sense how the  echo  works, it is either, or..
# There are POSIX echos, SysV echos, BSD echos...
#case "`echo "foo\c"`" in
#foo)   eopt='' ; eeol='\c' ;;
#*)     eopt='-n' ; eeol='' ;;
#esac

eopt='-n'
eeol=''

. $ZCONFIG || exit 1

case $POSTOFFICE in
/*)	;;
*)	echo "$0: panic!! can't initialize from $ZCONFIG"
	exit 1
	;;
esac

cd $POSTOFFICE	# So that possible cores are there..

if [ ! -d $MAILBOX ]; then
	echo "$0: panic!!  MAILBOX-variable does not point to a directory!  Verify $ZCONFIG!"
	exit 1
fi

# set up the default arguments
if [ "$1" = "start" -o "$1" = "reload" -o "$1" = "force-reload" -o $# = 0 ]; then
	set -$- router scheduler smtpserver
	echo $eopt "Starting Zmailer mail server: $eeol"
fi

KILL=

for op in $@
do
	shift
	case "$op" in
	stop|kill|nuke|kills|killr)	# print nothing
		;;
	*)	echo $eopt "${op} $eeol"
		;;
	esac
	case "$op" in
	router|outer)
			case $KILL in
			-*)	if [ -f $POSTOFFICE/.pid.$op ]; then
					$MAILBIN/router -k > /dev/null
#				else
#					echo $eopt "(warning: no .pid.$op file) $eeol"
				fi ;;
			*)
				if [ -f $POSTOFFICE/.freeze ]; then
					echo "Sorry, Zmailer is frozen, won't start anything until thawed!"
					exit 0
				fi
				$MAILBIN/router $ROUTEROPTIONS
				;;
			esac
			;;
#	oscheduler)
	osched*)
			case $KILL in
			-*)	kill $KILL `cat $POSTOFFICE/.pid.$op` ;;
			*)
				if [ -f $POSTOFFICE/.freeze ]; then
					echo "Sorry, Zmailer is frozen, won't start anything until thawed!"
					exit 0
				fi
				cd $POSTOFFICE/transport
				# must send signal to scheduler to make it
				# stop reading the directory for new files
				PIDFILE=.pid.scheduler
				if [ -f ../$PIDFILE ]; then
					PID=`cat ../$PIDFILE`
					kill -TERM -$PID 2>/dev/null
#				else
#					echo $eopt "(warning: no $PIDFILE file) $eeol"
				fi
				$MAILBIN/scheduler-old
				;;
			esac
			;;
	sched*|ched*)
			case $KILL in
			-*)	kill $KILL -`cat $POSTOFFICE/.pid.$op` ;;
			*)
				if [ -f $POSTOFFICE/.freeze ]; then
					echo "Sorry, Zmailer is frozen, won't start anything until thawed!"
					exit 0
				fi
				cd $POSTOFFICE/transport
				# must send signal to scheduler to make it
				# stop reading the directory for new files
				PIDFILE=.pid.scheduler
				if [ -f ../$PIDFILE ]; then
					PID=`cat ../$PIDFILE`
					kill -TERM -$PID 2>/dev/null
#				else
#					echo $eopt "(warning: no $PIDFILE file) $eeol"
				fi
				$MAILBIN/scheduler $SCHEDULEROPTIONS
				;;
			esac
			;;
	smtp*|mtp*)
			case $KILL in
			-*)	if [ -f $POSTOFFICE/.pid.$op ]; then
					kill $KILL `cat $POSTOFFICE/.pid.$op`
#				else
#					echo $eopt "(warning: no .pid.$op file) $eeol"
				fi ;;
			*)
				if [ -f $POSTOFFICE/.freeze ]; then
					echo "Sorry, Zmailer is frozen, won't start anything until thawed!"
					exit 0
				fi
				cd / ; $MAILBIN/smtpserver $SMTPOPTIONS
				;;
			esac
			;;
	newf|newfqdnaliases) $MAILBIN/router -f $MAILBIN/newfqdnaliases
			;;
	new|newaliases)     $MAILBIN/router -f $MAILBIN/newaliases
			;;
	new-route*)
			    $MAILBIN/newdb $MAILVAR/db/routes
			;;
	new-local*)
			    $MAILBIN/newdb $MAILVAR/db/localnames
			;;
	stop|kill|nuke|kills|killr)
			case $# in
			0)	exec $0 $op router scheduler smtpserver ;;
			esac
			case $op in
			kill|stop)
			    KILL="-TERM"
			    echo $eopt "Stopping Zmailer services: $eeol"
			    ;;
			nuke)
			    KILL="-KILL"
			    echo $eopt "Aborting Zmailer services: $eeol"
			    ;;
			esac
			;;
	resubmit)	(cd $POSTOFFICE/deferred &&
			 mv -i [0-9]* ../router) 2> /dev/null
			;;
	bootclean)	rm -f $POSTOFFICE/.pid.* 2> /dev/null
			;;
	cleanup)	if [ -d $POSTOFFICE/public -a -d $POSTOFFICE/postman ]
			then (cd $POSTOFFICE ;
			 find public -type f -mtime +2 -print |
				xargs rm -f
			 find postman -type f -mtime +7 -name '[0-9]*' -print |
				xargs rm -f
			)
			fi
			;;
	freeze)		touch $POSTOFFICE/.freeze
			;;
	unfr*|thaw)	rm -f $POSTOFFICE/.freeze
			;;
	restart)	/etc/init.d/zmailer-ssl stop && echo "Sleeping 5 seconds...." && sleep 5 && /etc/init.d/zmailer-ssl start
			;;
	*)		echo $0: unknown option: $op
			errflg=1
			;;
	esac
done
echo
case $errflg in
1)	echo Usage: $0 "[ router | scheduler | smtpserver | stop | kill | resubmit | bootclean | cleanup | freeze | thaw | reload | force-reload ]"
	exit 1
	;;
esac
exit 0
