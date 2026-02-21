<nul set /p ="#define GIT_VERSION " > src\gitversion.h
git describe --tags --long >> src\gitversion.h
