CC      = gcc
CFLAGS  = -O2 -Wall -Wextra -std=c11
LDFLAGS =

TARGET  = tracker
SRC     = $(wildcard *.c)

.PHONY: all clean install

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -f $(TARGET)

# Install as CGI binary + CSS.
# Run as root (or via sudo) after building.
install: $(TARGET)
	install -d /var/www/cgi-bin /var/www/root
	install -m 755 $(TARGET)    /var/www/cgi-bin/tracker
	install -m 644 tracker.css  /var/www/root/tracker.css
	# Create and secure the database
	touch /var/www/cgi-bin/tracker.db
	chown www-data:www-data /var/www/cgi-bin/tracker.db
	chmod 660 /var/www/cgi-bin/tracker.db
	@echo "Installed. Visit /cgi-bin/tracker/seed once to add default people."
