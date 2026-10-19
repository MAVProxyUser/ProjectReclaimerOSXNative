// Reclaimer.app's program (shim/tools/make-app.sh puts it at Contents/MacOS/Reclaimer): runs the bundle's
// Contents/Resources/launch.sh with /bin/bash. It exists because macOS privacy protection (TCC) silently refused a SCRIPT
// as the bundle's program access to a protected folder ("/bin/bash: shim/play.sh: Operation not permitted" when the
// install lives on the Desktop): a signed Mach-O app gets the normal one-time "Reclaimer would like to access files in
// your Desktop folder" prompt instead, and the game it starts inherits the permission.
#include <libgen.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    char exe[PATH_MAX], real[PATH_MAX], script[PATH_MAX + 64];
    uint32_t n = sizeof exe;
    if (_NSGetExecutablePath(exe, &n) || !realpath(exe, real)) { fprintf(stderr, "reclaimer: cannot find myself\n"); return 1; }
    char *macos = dirname(real);                      // .../Reclaimer.app/Contents/MacOS
    char contents[PATH_MAX]; snprintf(contents, sizeof contents, "%s", macos);
    char *c = dirname(contents);                      // .../Reclaimer.app/Contents
    snprintf(script, sizeof script, "%s/Resources/launch.sh", c);
    execl("/bin/bash", "bash", script, (char *)NULL);
    perror("reclaimer: /bin/bash");
    return 1;
}
