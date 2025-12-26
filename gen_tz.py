import csv, pathlib
rows = list(csv.reader(open('..\\tzdb_tmp\\zones.csv')))
lines = ['#pragma once', '', '#include <stdint.h>', '', 'struct IanaPosixEntry { const char *iana; const char *posix; };', '', 'static const IanaPosixEntry IANA_POSIX_DB[] PROGMEM = {']
for r in rows:
    lines.append('    {"%s", "%s"},' % (r[0].lower(), r[1]))
lines.append('};')
lines.append('')
lines.append('static const size_t IANA_POSIX_DB_COUNT = %d;' % len(rows))
path = pathlib.Path('firmware/config/IanaPosixDb.h')
path.write_text('\n'.join(lines))
print('written', path, len(rows))
