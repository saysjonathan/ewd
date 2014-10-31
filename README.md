# ewd
ewd is a simple worker queue for asynchronous background tasks.

## Synopsis
ewd is a job queue and worker management system. Queues are declared in a crontab-like file along with the maximum number of concurrent workers and the command the workers will run.

	$ cat /etc/ewd.conf
	images 5 /usr/local/bin/process_images
	stats 6 /usr/local/bin/generate_stats

Jobs in ewd are simply a list of arguments to pass to the command. Jobs can be added to the queue using a simple ASCII format, specifying the queue followed by the command arguments.

	$ echo "images --image test.jpg --format png --size=1024x768" | nc ewd.foo.com 8277

## Install
	git clone git://github.com/saysjonathan/ewd.git

Edit the `config.mk` file to customize the install paths. The prefix `/usr/local` is used by default.

To build:

	make

To install:

	make install

## License
MIT/X Consortium License

© 2014 Jonathan Boyett <jonathan@failingservers.com>
