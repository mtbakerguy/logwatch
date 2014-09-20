logwatch
========
Logwatch -- what is it?

Logwatch is a small framework for "tailing" a scripted set of logfiles and giving each
new line written to the file to a configurable lua function.  Its muse was scads of
annoying errors in /var/log/secure.log from brute force ssh attacks as well as the fact
that using lua to trivially create scriptable applications is just plain fun.  Likewise, I'm a
BSD guy so it's based on kqueue since I find it madly elegant.

It's been tested on Mac OSX (10.3.9) as well as NetBSD current, but I'd expect any
reasonably modern BSD to be fine.  Since it's a fun project tailored to my specific needs,
it's not full-featured.

General services provided by the main program:

       * a "magically named" script-based configuration mechanism allowing a user
         to specify files and their line handling functions.  See the "files2watch" variable
	 in ssh_login_filter for the syntax.
       * a kqueue-based eventloop to triggers lua events when a line of text is
         written or when the timer expires (see CLEAN_TIME macro in logwatch.cpp;
	 defaults to 5 seconds)
       * if underlying files are renamed or deleted, it will periodically attempt to reopen the
         file
       * SIGHUP cleans everything up and re-initializes

Key concepts in the main program:

    * openfiles contains a map of file descriptors to File* objects with active files
    * files2open contain a set of File* objects where we were unable to open the file
      name in question
         
API functions exported to the lua interpreter:

       * the "getinterface_address" function is provided to lua so a user can turn, say,
         "en0" into "192.168.1.1" (NB:  IPv4 support only)
       * the "getnicaddressbyaddress" function takes an IPv4 dotted quad and returns
         the IPv4 address of the appropriate nic. 

While the kevent/kqueue should be portable within the BSDs, it's unclear to me if
the above functions are.  Given the similarity of code between different /sbin/route
implementations, they probably are.

There's also an example script (ssh_login_filter) that temporarily blackholes ssh
attacks using ipfw commands.  Running for a couple of weeks, it's blocked between
2-3 attacks per day.

Finally, it would've been easier to do this in python, perl or even sh, but it wouldn't
have been nearly as much fun nor would I have learned anything about routing
sockets.

