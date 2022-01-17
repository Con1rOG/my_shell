#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <limits.h>

#define BUF_SIZE 1000
#define MAX_ARGS 1000
#define MAX_ENV_SIZE 10000

void handler(int sig)
{
    int wait_status = 1;
    int _exit_code = 0;
    while (wait_status && wait_status != -1) {
        int status = 0;
        wait_status = waitpid(-2, &status, 0);
        _exit_code = (WIFEXITED(status)) ? WEXITSTATUS(status)
            : ((WIFSIGNALED(status)) ? WTERMSIG(status) : 0);
    }
    _exit(_exit_code);
}

typedef struct Pipeline
{
    char **units; // элементы конвейера ( cmd1 | cmd2 | cmd3 )
    int count;    // количество элементов конвейера
} Pipeline;

// команда с аргументами и перенаправлениями (БЕЗ ; | || &&)
typedef struct Command
{
    char *command;           // команда shell ( ls, cat, wc, grep и т.д. )
    int argc;                // количество аргументов
    char *argv[MAX_ARGS];    // массив аргументов, вряд ли подадут больше 10000
    char file_in[PATH_MAX];  // перенаправление ввода
    char file_out[PATH_MAX]; // перенаправление вывода
    int out_mode;            // режим перенаправления ( O_TRUNC (>) или O_APPEND (>>) )
} Command;

// проверка ошибок в введённой строке
int CheckStr(char *str)
{
    if (!str) {
        return 0;
    }
    int len = strlen(str);
    char *tmp = strstr(str, "|");
    // если строка начинается или заканчивается '|'
    if ((tmp == str && str[1] != '|') || (tmp == str + len - 1)) {
        char msg[] = "Syntax error near \'|\'\n";
        write(2, msg, sizeof(msg) - 1);
        return 0;
    }
    // если строка начинается или заканчивается '||'
    tmp = strstr(str, "||");
    if ((tmp == str) || (tmp == str + len - 2)) {
        char msg[] = "Syntax error near \'||\'\n";
        write(2, msg, sizeof(msg) - 1);
        return 0;
    }
    // если строка начинается с '&'
    tmp = strstr(str, "&");
    if (tmp == str && str[1] != '&') {
        char msg[] = "Syntax error near \'&\'\n";
        write(2, msg, sizeof(msg) - 1);
        return 0;
    }
    // если строка начинается или заканчивается '&&'
    tmp = strstr(str, "&&");
    if ((tmp == str) || (tmp == str + len - 2)) {
        char msg[] = "Syntax error near \'&&\'\n";
        write(2, msg, sizeof(msg) - 1);
        return 0;
    }
    // если строка начинается или заканчивается ';'
    tmp = strstr(str, ";");
    if ((tmp == str) || (tmp == str + len - 1)) {
        char msg[] = "Syntax error near \';\'\n";
        write(2, msg, sizeof(msg) - 1);
        return 0;
    }
    // если строка начинается или заканчивается '<'
    tmp = strstr(str, "<");
    if ((tmp == str) || (tmp == str + len - 1)) {
        char msg[] = "Syntax error near \'<\'\n";
        write(2, msg, sizeof(msg) - 1);
        return 0;
    }
    // если строка начинается или заканчивается '>'
    tmp = strstr(str, ">");
    if ((tmp == str) || (tmp == str + len - 1)) {
        char msg[] = "Syntax error near \'>\'\n";
        write(2, msg, sizeof(msg) - 1);
        return 0;
    }
    return 1;
}

char *StringReader(void)
{
    unsigned int size = BUF_SIZE;
    char *str = malloc(size * sizeof(char));
    int cnt = 0;
    char c = 0;
    while (read(0, &c, 1) == 1) {
        if (cnt + 3 >= size) {
            size *= 2;
            str = realloc(str, size * sizeof(char));
        }
        if ((cnt == 0 && isspace(c)) || (isspace(c) && str[cnt - 1])) {
            continue;
        }
        if (cnt == 0) {
            str[cnt] = c;
            continue;
        }
    }
    return str;
}

char *SpacesDeleter(char *my_string)
{
    if (!my_string) {
        return NULL;
    }
    int begin = 0;
    while (my_string[begin] == ' ' || my_string[begin] == '\t') {
        begin++;
    }

    int len = strlen(my_string);
    if (begin != len - 1) {
        while (my_string[len - 1] == ' ' || my_string[len - 1] == '\t') {
            len--;
            my_string[len] = '\0';
        }
    }

    int cnt = begin;
    while (my_string[cnt + 1] && cnt < len) {
        if ((my_string[cnt] == ' ' && my_string[cnt + 1] == ' ') ||
            (my_string[cnt] == ' ' && my_string[cnt + 1] == '\t') ||
            (my_string[cnt] == '\t' && my_string[cnt + 1] == '\t') ||
            (my_string[cnt] == '\t' && my_string[cnt + 1] == ' ')) {
            cnt++;
        }
    }

    char *new_str = calloc(len - cnt, sizeof(char));

    int j = 0;
    for (int i = begin; i < len - 1; ++i) {
        if (my_string[i] == ' ' || my_string[i] == '\t') {
            new_str[j] = ' ';
            while (my_string[i + 1] == ' ' || my_string[i + 1] == '\t') {
                i++;
            }
        } else {
            new_str[j] = my_string[i];
        }
        j++;
    }
    free(my_string);
    return new_str;
}

void FreeCmd(Command *cmd)
{
    if (cmd) {
        if (cmd->command) {
            free(cmd->command);
        }
        for (int i = 0; i < cmd->argc; ++i) {
            if (cmd->argv[i]) {
                free(cmd->argv[i]);
            }
        }
        free(cmd);
    }
}

Command *ParseCmd(char *unit)
{
    // новая пустая команда
    Command *cmd = malloc(sizeof(Command));
    cmd->argc = 0;
    cmd->command = NULL;
    for (int i = 0; i < PATH_MAX; ++i) {
        cmd->file_in[i] = '\0';
        cmd->file_out[i] = '\0';
    }
    cmd->out_mode = 0;

    // divide cmd, args, <, >, >>
    char *tmp_space = "1";
    char *tmp_unit = unit;

    // пока находим пробел делаем цикл
    while (tmp_space) {
        tmp_space = strstr(tmp_unit, " ");
        if (tmp_space) {
            // временно отделяем часть строки до пробела
            tmp_space[0] = '\0';
            // начало строки до первого пробела - всегда сама команда
            // поэтому её записываем в первую очередь, если еще не записали (cmd->command == NULL)
            if (cmd->command == NULL) {
                cmd->command = calloc(strlen(tmp_unit) + 1, sizeof(char));
                strcpy(cmd->command, tmp_unit);

                // argv[0] - сама команда по стандарту
                cmd->argc++;
                cmd->argv[cmd->argc - 1] = malloc((strlen(cmd->command) + 1) * sizeof(char *));
                strcpy(cmd->argv[0], cmd->command);
                // argv[argc] - последний аргумент, NULL по стандарту
                cmd->argv[cmd->argc] = NULL;
                // пропускаем наш пробел
                tmp_unit = tmp_space + 1;
            }

            // если встретили перенаправление ввода
            if (tmp_space[1] == '<') {
                // " < "
                // находим имя файла
                char *start_filename = tmp_space + 3;
                char *end_filename = strstr(start_filename, " ");
                if (end_filename) {
                    end_filename[0] = '\0';
                }
                strcpy(cmd->file_in, start_filename);

                // если не нашли пробел, значит строка заканчивается на имени файла
                if (end_filename) {
                    end_filename[0] = ' ';
                    tmp_unit = end_filename;
                } else {
                    tmp_unit = start_filename + strlen(start_filename);
                }
            }
            // если встретили перенаправление вывода (перезапись)
            else if (tmp_space[1] == '>' && tmp_space[2] != '>') {
                // " > "
                // находим имя файла
                char *start_filename = tmp_space + 3;
                char *end_filename = strstr(start_filename, " ");
                if (end_filename) {
                    end_filename[0] = '\0';
                }
                strcpy(cmd->file_out, start_filename);
                cmd->out_mode = O_TRUNC;

                // если не нашли пробел, значит строка заканчивается на имени файла
                if (end_filename) {
                    end_filename[0] = ' ';
                    tmp_unit = end_filename;
                } else {
                    tmp_unit = start_filename + strlen(start_filename);
                }
            }
            // если встретили перенаправление вывода (добавление)
            else if (tmp_space[1] == '>' && tmp_space[2] == '>') {
                // " >> "
                // находим имя файла
                char *start_filename = tmp_space + 4;
                char *end_filename = strstr(start_filename, " ");
                if (end_filename) {
                    end_filename[0] = '\0';
                }
                strcpy(cmd->file_out, start_filename);
                cmd->out_mode = O_APPEND;

                // если не нашли пробел, значит строка заканчивается на имени файла
                if (end_filename) {
                    end_filename[0] = ' ';
                    tmp_unit = end_filename;
                } else {
                    tmp_unit = start_filename + strlen(start_filename);
                }
            }
            // если не нашли нет перенаправлений, то обычный аргумент
            else {
                // " arg "
                // находим сам аргумент
                char *start_arg = tmp_space + 1;
                char *end_arg = strstr(start_arg, " ");
                if (end_arg) {
                    end_arg[0] = '\0';
                }

                char buf[MAX_ENV_SIZE] = {0};
                if (start_arg[0] == '$') {
                    strcpy(buf, getenv(start_arg + 1));
                }

                // увеличиваем число аргументов на 1
                cmd->argc++;
                // если аргументов стало больше 10000 - игнорируем
                if (cmd->argc <= MAX_ARGS) {

                    if (buf[0]) {
                        cmd->argv[cmd->argc - 1] = malloc((strlen(buf) + 1) * sizeof(char));
                        strcpy(cmd->argv[cmd->argc - 1], buf);
                    } else {
                        cmd->argv[cmd->argc - 1] = malloc((strlen(start_arg) + 1) * sizeof(char));
                        strcpy(cmd->argv[cmd->argc - 1], start_arg);
                    }
                    cmd->argv[cmd->argc] = NULL;
                }

                if (end_arg) {
                    end_arg[0] = ' ';
                    tmp_unit = end_arg;
                } else {
                    tmp_unit = start_arg + strlen(start_arg);
                }
            }
            tmp_space[0] = ' ';
        }
    }
    // если в строке не было пробелов
    if (cmd->command == NULL) {
        cmd->command = calloc(strlen(tmp_unit) + 1, sizeof(char));
        strcpy(cmd->command, tmp_unit);

        cmd->argc++;
        cmd->argv[cmd->argc - 1] = malloc((strlen(cmd->command) + 1) * sizeof(char *));
        strcpy(cmd->argv[0], cmd->command);
        cmd->argv[cmd->argc] = NULL;
    }
    //  или обрабатывается последний аргумент
    else if (strlen(tmp_unit)) {
        // " arg "
        char *start_arg = tmp_unit;
        char buf[MAX_ENV_SIZE] = {0};
        if (start_arg[0] == '$') {
            strcpy(buf, getenv(start_arg + 1));
        }

        // увеличиваем число аргументов на 1
        cmd->argc++;
        // если аргументов стало больше 10000 - игнорируем
        if (cmd->argc <= MAX_ARGS) {
            cmd->argv[cmd->argc - 1] = malloc((strlen(start_arg) + 1) * sizeof(char));
            if (buf[0]) {
                strcpy(cmd->argv[cmd->argc - 1], buf);
            } else {
                strcpy(cmd->argv[cmd->argc - 1], start_arg);
            }
            cmd->argv[cmd->argc] = NULL;
        }
    }
    // fprintf(stderr, "\nParsed cmd\n\nКоманда: %s\n", cmd->command);
    // fflush(stderr);
    // for (int i = 0; i < cmd->argc; ++i)
    // {
    //     fprintf(stderr, "argv[%d]: %s\n", i, cmd->argv[i]);
    //     fflush(stderr);
    // }
    // fprintf(stderr, "argv[%d]: %s\n", cmd->argc, cmd->argv[cmd->argc]);
    // fflush(stderr);
    // fprintf(stderr, "< %s\n> %s\n", cmd->file_in, cmd->file_out);
    // fflush(stderr);
    return cmd;
}

int cd(char *pth)
{
    if (!pth || pth[0] == '~') {
        if (!chdir(getenv("HOME"))) {
            return 0;
        }
        perror("cd");
        return 1;
    }

    char cwd[PATH_MAX] = {0};
    if (pth[0] != '/') {
        char path[2 * PATH_MAX] = {0};
        getcwd(cwd, sizeof(cwd));
        snprintf(path, sizeof(path), "%s/%s", cwd, pth);
        if (!chdir(path)) {
            return 0;
        }
        perror("cd");
        return 1;
    } else {
        if (!chdir(pth)) {
            return 0;
        }
        perror("cd");
        return 1;
    }
    return 0;
}

int ProcessCmd(Command *cmd, int background_flag)
{
    if (!strcmp(cmd->command, "cd")) {
        return cd(cmd->argv[1]);
    }
    // если фоновый запуск
    if (background_flag) {
        pid_t pid = fork();
        if (!pid) {
            // делаем внука
            pid_t pid_grandson = fork();
            if (!pid_grandson) {
                // перенаправляем ввод из пустого файла
                int fd_dev_null = open("/dev/null", O_RDWR);
                dup2(fd_dev_null, 0);
                close(fd_dev_null);

                // если определено перенаправление с помощью "<"|">"|">>" - выполняем
                if (strlen(cmd->file_in)) {
                    int fd = open(cmd->file_in, O_RDONLY);
                    if (fd == -1) {
                        char msg[] = "Ошибка перенаправления\n";
                        write(2, msg, sizeof(msg) - 1);
                        _exit(1);
                    }
                    dup2(fd, 0);
                    close(fd);
                }
                if (strlen(cmd->file_out)) {
                    int fd = open(cmd->file_out, O_WRONLY | O_CREAT | cmd->out_mode, 0640);
                    dup2(fd, 1);
                    close(fd);
                }
                // игнорируем SIGINT (ctrl-C)
                signal(SIGINT, SIG_IGN);
                execvp(cmd->command, cmd->argv);
                perror("execvp");
                _exit(1);
            }
            _exit(0);
        }
        waitpid(pid, NULL, 0);
        return 0;
    }
    // обычный запуск
    else {
        // делаем сына
        pid_t pid = fork();
        if (!pid) {
            // перенаправляем если надо
            if (strlen(cmd->file_in)) {
                int fd = open(cmd->file_in, O_RDONLY);
                if (fd == -1) {
                    char msg[] = "Ошибка перенаправления\n";
                    write(2, msg, sizeof(msg) - 1);
                    _exit(1);
                }
                dup2(fd, 0);
                close(fd);
            }
            if (strlen(cmd->file_out)) {
                int fd = open(cmd->file_out, O_WRONLY | O_CREAT | cmd->out_mode, 0640);
                dup2(fd, 1);
                close(fd);
            }
            execvp(cmd->command, cmd->argv);
            perror("execvp");
            _exit(1);
        }
        // возвращаем код выхода, чтобы использовать в логических операциях ( && и || )
        int status;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status)) {
            return WEXITSTATUS(status);
        }
        if (WIFSIGNALED(status)) {
            return 128 + WTERMSIG(status);
        }
        return 1;
    }
}

int ProcessSemicolonUnit(char *unit)
{
    // &

    char *tmp_background = "1";
    char *tmp_cmd = unit;

    int _exit_code = 0;
    int background_flag = 0;
    // пока находим "&" делаем цикл
    while (tmp_background) {
        // ls & ls & ls
        // ls & ls & ls &
        tmp_background = strstr(tmp_cmd, " &");
        if (tmp_background) {
            // если нашли "&", то инициируем фоновый запуск
            background_flag = 1;

            // временно отделяем команду от остальной строки
            tmp_background[0] = '\0';
            Command *cmd = ParseCmd(tmp_cmd);
            // fprintf(stderr, "Запустили & cmd = %s\n", tmp_cmd);
            // fflush(stderr);

            // записываем код возврата для логических операций ( && и || )
            _exit_code = ProcessCmd(cmd, background_flag);
            FreeCmd(cmd);
            tmp_cmd = tmp_background + 2;
            // возвращаем строку в исходное состояние
            tmp_background[0] = ' ';
        }
    }
    // если строка не пустая, то "&" в строке уже нет
    if (strlen(tmp_cmd)) {
        // инициируем обычный запуск
        Command *cmd = ParseCmd(tmp_cmd);
        // записываем код возврата для логических операций ( && и || )
        _exit_code = ProcessCmd(cmd, background_flag);
        FreeCmd(cmd);
    }
    return _exit_code;
}

int ProcessLogicalUnit(char *unit)
{
    // ;

    char *tmp_semicolon = "1";
    char *tmp_unit = unit;
    int _exit_code = 0;

    // пока находим ";" делаем цикл
    while (tmp_semicolon) {
        tmp_semicolon = strstr(tmp_unit, " ; ");
        // если нашли ";"
        if (tmp_semicolon) {
            // отделяем unit(блок команд) от остальной строки
            tmp_semicolon[0] = '\0';
            // записываем код возврата для логических операций ( && и || )
            _exit_code = ProcessSemicolonUnit(tmp_unit);
            // возвращаем строку в исходное состояние
            tmp_semicolon[0] = ' ';
            tmp_unit = tmp_semicolon + 3;
        } else {
            // записываем код возврата для логических операций ( && и || )
            _exit_code = ProcessSemicolonUnit(tmp_unit);
        }
    }
    return _exit_code;
}

int ProcessPipelineUnit(char *unit)
{
    // && ||

    char *tmp_and = "1";
    char *tmp_or = "1";
    char *tmp_unit = unit;
    int _exit_code = 0;
    // храним предыдущее значение логического выражения
    int prev_flag = 1;

    // пока находим "&&" или "||" делаем цикл
    while (tmp_and || tmp_or) {
        tmp_and = strstr(tmp_unit, " && ");
        tmp_or = strstr(tmp_unit, " || ");

        // если нашли && раньше чем || или нашли && и не нашли ||
        if (tmp_and && (!tmp_or || tmp_and < tmp_or)) {
            tmp_and[0] = '\0';

            // ls ; cat < 2.txt && ls

            // ls ; cat < 2.txt
            // (&&) flag = 1
            // ls

            // ProcessLogicalUnit("ls ; cat < 2.txt")
            // ProcessLogicalUnit("ls")
            if (prev_flag) {
                // записываем код возврата
                _exit_code = ProcessLogicalUnit(tmp_unit);
            }
            tmp_and[0] = ' ';
            tmp_unit = tmp_and + 4;
            // проверяем код возврата - если успех(0), то логическое выражение истинно, иначе ложно
            prev_flag = (_exit_code == 0);
        }
        // если нашли || раньше чем && или нашли || и не нашли &&
        else if (tmp_or && (!tmp_and || tmp_or < tmp_and)) {
            tmp_or[0] = '\0';

            if (prev_flag) {
                // записываем код возврата
                _exit_code = ProcessLogicalUnit(tmp_unit);
            }
            tmp_or[0] = ' ';
            tmp_unit = tmp_or + 4;
            // проверяем код возврата - если ошибка(не 0), то логическое выражение истинно, иначе ложно
            prev_flag = (_exit_code != 0);
        }
        // если на нашли && или ||
        // то просто проверяем значение логического выражения
        else if (prev_flag) {
            ProcessLogicalUnit(tmp_unit);
        }
    }
    return _exit_code;
}

void FreePipeline(Pipeline *pipeline)
{
    if (pipeline) {
        if (pipeline->units) {
            for (int i = 0; i < pipeline->count; ++i) {
                if (pipeline->units[i]) {
                    free(pipeline->units[i]);
                }
            }
            free(pipeline->units);
        }
        free(pipeline);
    }
}

Pipeline *PipelineDivider(char *my_string)
{
    // |

    Pipeline *pipeline = malloc(sizeof(Pipeline));

    pipeline->count = 0;
    char *tmp = my_string;
    // считаем количество "|"
    while ((tmp = strstr(tmp, " | "))) {
        pipeline->count++;
        tmp++;
    }

    // выделяем память на count + 1 units(блоков команд)
    pipeline->units = malloc((pipeline->count + 1) * sizeof(char *));
    tmp = my_string;
    for (int i = 0; i < pipeline->count; i++) {
        char *divider = strstr(tmp, " | ");
        if (divider) {
            divider[0] = '\0';
            pipeline->units[i] = calloc(strlen(tmp) + 1, sizeof(char));
            strcpy(pipeline->units[i], tmp);
            divider[0] = ' ';

            tmp = divider + 3;
        }
    }
    // последний блок записывается отдельно
    pipeline->units[pipeline->count] = calloc(strlen(tmp) + 1, sizeof(char));
    strcpy(pipeline->units[pipeline->count], tmp);
    pipeline->count++;
    return pipeline;
}

int ProcessPipeline(Pipeline *pipeline)
{
    int in = 0;
    int _exit_code = 0;
    if (pipeline->count == 1) {
        _exit_code = ProcessPipelineUnit(pipeline->units[0]);
        return _exit_code;
    }

    // массив пидов чтобы потом их ждать
    pid_t *pipeline_pids = calloc(pipeline->count, sizeof(pid_t));
    // stdin -> cmd1 -> [pipe1] -> cmd2 -> [pipe2] -> cmd3 -> [pipe3] -> ....
    // cmd1 [args] | cmd2 [args] | cmd3 [args]
    for (int i = 0; i < pipeline->count - 1; ++i) {
        int pipe_fd[2];
        // создаем пайп(канал)
        pipe(pipe_fd);

        // создаем сына для одного блока
        pipeline_pids[i] = fork();
        if (pipeline_pids[i] == 0) {
            close(pipe_fd[0]);
            free(pipeline_pids);
            // перенаправляем ввод и вывод в пайп(канал)
            if (in) {
                dup2(in, 0);
                close(in);
            }
            if (pipe_fd[1] != 1) {
                dup2(pipe_fd[1], 1);
                close(pipe_fd[1]);
            }
            // выполняем блок
            _exit_code = ProcessPipelineUnit(pipeline->units[i]);
            close(pipe_fd[1]);
            _exit(_exit_code);
        }
        // закрываем старый пайп
        close(pipe_fd[1]);
        close(in);
        // сохраняем дескриптор на чтение из нового пайпа
        in = pipe_fd[0];
        // in = pipe1[0] -> in = pipe2[0] -> in = pipe3[0]
    }
    // последний блок выполняется отдельно без перенаправления вывода
    pipeline_pids[pipeline->count - 1] = fork();
    if (pipeline_pids[pipeline->count - 1] == 0) {
        // перенаправляем ввод
        if (in) {
            dup2(in, 0);
            close(in);
        }
        free(pipeline_pids);
        _exit_code = ProcessPipelineUnit(pipeline->units[pipeline->count - 1]);
        _exit(_exit_code);
    }
    close(in);
    // ждем завершения всех блоков
    int status = 0;
    for (int i = 0; i < pipeline->count; ++i) {
        waitpid(pipeline_pids[i], &status, 0);
        if (WIFEXITED(status)) {
            _exit_code = WEXITSTATUS(status);
        }
        free(pipeline_pids);
    }
    return _exit_code;
}

int MyShell(char *str)
{
    int _exit_code = 1;
    // проверяем строку на некорректный ввод
    if (CheckStr(str)) {
        // если строка не пустая, то запускаем её обработку
        if (strlen(str)) {
            Pipeline *pipeline = PipelineDivider(str);
            if (pipeline) {
                // создаем сына для выполнения конвейера и дальнейшей обработки
                _exit_code = ProcessPipeline(pipeline);
                FreePipeline(pipeline);
            }
        }
    }
    return _exit_code;
}

int main(void)
{
    signal(SIGQUIT, handler);
    signal(SIGINT, handler);
    signal(SIGHUP, handler);
    setbuf(stdin, NULL);
    int _exit_code = 0;
    pid_t pid = fork();
    if (!pid) {
        // выводим красивую подсказку с именем текущей директории
        char pwd[PATH_MAX] = {0};
        // получаем полный путь к директории
        getcwd(pwd, sizeof(pwd));
        char *name = strrchr(pwd, '/');
        // выводим последнюю часть пути, то есть имя текущей директории
        char msg[PATH_MAX + 6] = {0};
        snprintf(msg, sizeof(msg), "[%s]>>> ", &name[1]);
        write(2, msg, strlen(msg));
        // читаем строку
        char *str = StringReader();
        printf("read");
        // добавляем недостающие пробелы
        str = SpacesAdder(str);
        printf("read");
        // удаляем лишние пробелы
        str = SpacesDeleter(str);
        printf("read");
        _exit_code = MyShell(str);
        free(str);
    }
    waitpid(pid, NULL, 0);
    return _exit_code;
}