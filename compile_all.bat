@echo off
echo Компиляция проекта Battleship...

echo Компиляция сервера...
g++ battleship_server.cpp battleship.cpp -o battleship_server.exe -lws2_32 -std=c++17
if errorlevel 1 (
    echo Ошибка компиляции сервера!
    pause
    exit /b 1
)

echo Компиляция клиента...
g++ battleship_client.cpp battleship.cpp -o battleship_client.exe -lws2_32 -std=c++17
if errorlevel 1 (
    echo Ошибка компиляции клиента!
    pause
    exit /b 1
)

echo Успешно!
echo Созданы файлы:
dir *.exe
echo.
echo Инструкция запуска:
echo 1. Запустите battleship_server.exe
echo 2. Запустите дважды battleship_client.exe localhost [порт]
pause