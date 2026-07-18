#include "dispatch_cli_internal.h"

static void print_completion_candidate_kinds(void) {
    puts("commands");
    puts("candidate-kinds");
    puts("tasks");
    puts("groups");
    puts("agents");
    puts("workspaces");
}

static void print_completion_usage(void) {
    fprintf(stderr,
            "Usage: dispatch completion candidates "
            "commands|candidate-kinds|tasks|groups|agents|workspaces\n");
    fprintf(stderr, "       dispatch completion bash\n");
    fprintf(stderr, "       dispatch completion fish\n");
    fprintf(stderr, "       dispatch completion zsh\n");
    fprintf(stderr, "       dispatch completion install fish|bash|zsh\n");
}

static int cmd_completion_candidates(int argc, char **argv) {
    if (argc != 4) {
        print_completion_usage();
        return 1;
    }

    const char *kind = argv[3];
    if (strcmp(kind, "commands") == 0) {
        for (int i = 0; commands[i].name != NULL; i++)
            puts(commands[i].name);
        return 0;
    }

    if (strcmp(kind, "candidate-kinds") == 0) {
        print_completion_candidate_kinds();
        return 0;
    }

    DispatchBoard board;
    if (!load_board_or_error(&board))
        return 1;

    if (strcmp(kind, "tasks") == 0) {
        for (size_t i = 0; i < board.tasks.count; i++)
            puts(board.tasks.items[i].id);
    } else if (strcmp(kind, "groups") == 0) {
        for (size_t i = 0; i < board.groups.count; i++)
            puts(board.groups.items[i].id);
    } else if (strcmp(kind, "agents") == 0) {
        for (size_t i = 0; i < board.agents.count; i++)
            puts(board.agents.items[i].name);
    } else if (strcmp(kind, "workspaces") == 0) {
        for (size_t i = 0; i < board.workspaces.count; i++) {
            DispatchWorkspace *workspace = &board.workspaces.items[i];
            if (workspace->state != DISPATCH_WORKSPACE_REMOVED)
                puts(workspace->id);
        }
    } else {
        dispatch_board_free(&board);
        print_completion_usage();
        return 1;
    }

    dispatch_board_free(&board);
    return 0;
}

static int cmd_completion_zsh(int argc, char **argv) {
    (void)argv;
    if (argc != 3) {
        print_completion_usage();
        return 1;
    }

    fputs(
        "#compdef dispatch\n"
        "\n"
        "_dispatch_candidate_values() {\n"
        "  dispatch completion candidates \"$1\" 2>/dev/null\n"
        "}\n"
        "\n"
        "_dispatch_compadd_candidates() {\n"
        "  local -a values\n"
        "  values=(\"${(@f)$(_dispatch_candidate_values \"$1\")}\")\n"
        "  (( ${#values} )) && compadd -- \"${values[@]}\"\n"
        "}\n"
        "\n"
        "_dispatch_completion_command() {\n"
        "  local -a subcommands kinds\n"
        "  subcommands=(candidates bash fish zsh install)\n"
        "  kinds=(\"${(@f)$(_dispatch_candidate_values candidate-kinds)}\")\n"
        "\n"
        "  if (( CURRENT == 3 )); then\n"
        "    compadd -- \"${subcommands[@]}\"\n"
        "  elif [[ ${words[3]} == candidates && CURRENT == 4 ]]; then\n"
        "    compadd -- \"${kinds[@]}\"\n"
        "  fi\n"
        "}\n"
        "\n"
        "_dispatch_group_command() {\n"
        "  if (( CURRENT == 3 )); then\n"
        "    compadd -- add ready\n"
        "  elif [[ ${words[3]} == ready && CURRENT == 4 ]]; then\n"
        "    _dispatch_compadd_candidates groups\n"
        "  fi\n"
        "}\n"
        "\n"
        "_dispatch_task_command() {\n"
        "  if (( CURRENT == 3 )); then\n"
        "    compadd -- add delete\n"
        "  elif [[ ${words[3]} == delete && CURRENT == 4 ]]; then\n"
        "    _dispatch_compadd_candidates tasks\n"
        "  fi\n"
        "}\n"
        "\n"
        "_dispatch_dep_command() {\n"
        "  if (( CURRENT == 3 )); then\n"
        "    compadd -- add remove\n"
        "  elif (( CURRENT == 4 || CURRENT == 5 )); then\n"
        "    _dispatch_compadd_candidates tasks\n"
        "  fi\n"
        "}\n"
        "\n"
        "_dispatch_commit_command() {\n"
        "  if (( CURRENT == 3 )); then\n"
        "    compadd -- add list show\n"
        "  elif [[ ${words[3]} == add || ${words[3]} == list || ${words[3]} == show ]] && "
        "(( CURRENT == 4 )); then\n"
        "    _dispatch_compadd_candidates tasks\n"
        "  fi\n"
        "}\n"
        "\n"
        "_dispatch_agent_command() {\n"
        "  if (( CURRENT == 3 )); then\n"
        "    compadd -- create list show command session resume archive restore\n"
        "  elif [[ ${words[3]} == show || ${words[3]} == command || ${words[3]} == session || ${words[3]} == resume || ${words[3]} == archive || ${words[3]} == restore ]] && "
        "(( CURRENT == 4 )); then\n"
        "    _dispatch_compadd_candidates agents\n"
        "  fi\n"
        "}\n"
        "\n"
        "_dispatch_workspace_command() {\n"
        "  if (( CURRENT == 3 )); then\n"
        "    compadd -- create list show remove prune\n"
        "  else\n"
        "    case ${words[3]} in\n"
        "      create)\n"
        "        (( CURRENT == 4 )) && _dispatch_compadd_candidates tasks\n"
        "        ;;\n"
        "      show|remove)\n"
        "        (( CURRENT == 4 )) && _dispatch_compadd_candidates workspaces\n"
        "        ;;\n"
        "    esac\n"
        "  fi\n"
        "}\n"
        "\n"
        "_dispatch() {\n"
        "  local -a commands\n"
        "  commands=(\"${(@f)$(_dispatch_candidate_values commands)}\")\n"
        "\n"
        "  if (( CURRENT == 2 )); then\n"
        "    compadd -- \"${commands[@]}\"\n"
        "    return\n"
        "  fi\n"
        "\n"
        "  case ${words[2]} in\n"
        "    show|start|finish|review|ready)\n"
        "      (( CURRENT == 3 )) && _dispatch_compadd_candidates tasks\n"
        "      ;;\n"
        "    list)\n"
        "      if (( CURRENT == 3 )); then\n"
        "        compadd -- all\n"
        "        _dispatch_compadd_candidates groups\n"
        "      elif [[ ${words[3]} == all && CURRENT == 4 ]]; then\n"
        "        _dispatch_compadd_candidates groups\n"
        "      fi\n"
        "      ;;\n"
        "    group)\n"
        "      _dispatch_group_command\n"
        "      ;;\n"
        "    task)\n"
        "      _dispatch_task_command\n"
        "      ;;\n"
        "    dep)\n"
        "      _dispatch_dep_command\n"
        "      ;;\n"
        "    commit)\n"
        "      _dispatch_commit_command\n"
        "      ;;\n"
        "    agent)\n"
        "      _dispatch_agent_command\n"
        "      ;;\n"
        "    workspace)\n"
        "      _dispatch_workspace_command\n"
        "      ;;\n"
        "    completion)\n"
        "      _dispatch_completion_command\n"
        "      ;;\n"
        "  esac\n"
        "}\n"
        "\n"
        "_dispatch \"$@\"\n",
        stdout);
    return 0;
}

static int cmd_completion_bash(int argc, char **argv) {
    (void)argv;
    if (argc != 3) {
        print_completion_usage();
        return 1;
    }

    fputs(
        "_dispatch_candidate_values() {\n"
        "  dispatch completion candidates \"$1\" 2>/dev/null\n"
        "}\n"
        "\n"
        "_dispatch_complete_words() {\n"
        "  COMPREPLY=( $(compgen -W \"$1\" -- \"$cur\") )\n"
        "}\n"
        "\n"
        "_dispatch_complete_candidates() {\n"
        "  _dispatch_complete_words \"$(_dispatch_candidate_values \"$1\")\"\n"
        "}\n"
        "\n"
        "_dispatch_complete() {\n"
        "  local cur prev cmd sub\n"
        "  cur=\"${COMP_WORDS[COMP_CWORD]}\"\n"
        "  prev=\"${COMP_WORDS[COMP_CWORD-1]}\"\n"
        "  cmd=\"${COMP_WORDS[1]}\"\n"
        "  sub=\"${COMP_WORDS[2]}\"\n"
        "  COMPREPLY=()\n"
        "\n"
        "  if (( COMP_CWORD == 1 )); then\n"
        "    _dispatch_complete_candidates commands\n"
        "    return\n"
        "  fi\n"
        "\n"
        "  if [[ ${cur} == --* ]]; then\n"
        "    case \"$cmd\" in\n"
        "      agent)\n"
        "        case \"$sub\" in\n"
        "          create) _dispatch_complete_words \"--name --runner --model "
        "--no-run-script --print-command\" ;;\n"
        "          list) _dispatch_complete_words \"--all\" ;;\n"
        "          command|resume) _dispatch_complete_words \"--print-command\" ;;\n"
        "        esac\n"
        "        ;;\n"
        "      workspace)\n"
        "        case \"$sub\" in\n"
        "          create) _dispatch_complete_words \"--actor --repo --dir "
        "--branch --sequence\" ;;\n"
        "          remove) _dispatch_complete_words \"--force\" ;;\n"
        "          prune) _dispatch_complete_words \"--done --stale --dry-run\" ;;\n"
        "        esac\n"
        "        ;;\n"
        "      group) [[ $sub == ready ]] && _dispatch_complete_words "
        "\"--actor --no-review\" ;;\n"
        "      task) [[ $sub == delete ]] && _dispatch_complete_words "
        "\"--force\" || _dispatch_complete_words \"--description --actor --no-review\" ;;\n"
        "      commit) [[ $sub == add ]] && _dispatch_complete_words \"--actor\" ;;\n"
        "      ready) _dispatch_complete_words \"--actor --no-review\" ;;\n"
        "      start|finish|review) _dispatch_complete_words \"--actor\" ;;\n"
        "    esac\n"
        "    return\n"
        "  fi\n"
        "\n"
        "  case \"$cmd\" in\n"
        "    completion)\n"
        "      if (( COMP_CWORD == 2 )); then\n"
        "        _dispatch_complete_words \"candidates bash fish zsh install\"\n"
        "      elif [[ $sub == candidates && $COMP_CWORD -eq 3 ]]; then\n"
        "        _dispatch_complete_candidates candidate-kinds\n"
        "      elif [[ $sub == install && $COMP_CWORD -eq 3 ]]; then\n"
        "        _dispatch_complete_words \"fish bash zsh\"\n"
        "      fi\n"
        "      ;;\n"
        "    show|start|finish|review|ready)\n"
        "      (( COMP_CWORD == 2 )) && _dispatch_complete_candidates tasks\n"
        "      ;;\n"
        "    list)\n"
        "      if (( COMP_CWORD == 2 )); then\n"
        "        _dispatch_complete_words \"all $(_dispatch_candidate_values groups)\"\n"
        "      elif [[ $sub == all && $COMP_CWORD -eq 3 ]]; then\n"
        "        _dispatch_complete_candidates groups\n"
        "      fi\n"
        "      ;;\n"
        "    group)\n"
        "      if (( COMP_CWORD == 2 )); then\n"
        "        _dispatch_complete_words \"add ready\"\n"
        "      elif [[ $sub == ready && $COMP_CWORD -eq 3 ]]; then\n"
        "        _dispatch_complete_candidates groups\n"
        "      fi\n"
        "      ;;\n"
        "    task)\n"
        "      if (( COMP_CWORD == 2 )); then\n"
        "        _dispatch_complete_words \"add delete\"\n"
        "      elif [[ $sub == delete && $COMP_CWORD -eq 3 ]]; then\n"
        "        _dispatch_complete_candidates tasks\n"
        "      fi\n"
        "      ;;\n"
        "    dep)\n"
        "      if (( COMP_CWORD == 2 )); then\n"
        "        _dispatch_complete_words \"add remove\"\n"
        "      elif (( COMP_CWORD == 3 || COMP_CWORD == 4 )); then\n"
        "        _dispatch_complete_candidates tasks\n"
        "      fi\n"
        "      ;;\n"
        "    commit)\n"
        "      if (( COMP_CWORD == 2 )); then\n"
        "        _dispatch_complete_words \"add list show\"\n"
        "      elif [[ ( $sub == add || $sub == list || $sub == show ) && $COMP_CWORD -eq 3 ]]; then\n"
        "        _dispatch_complete_candidates tasks\n"
        "      fi\n"
        "      ;;\n"
        "    agent)\n"
        "      if (( COMP_CWORD == 2 )); then\n"
        "        _dispatch_complete_words \"create list show command session resume archive restore\"\n"
        "      elif [[ ( $sub == show || $sub == command || $sub == session || $sub == resume || $sub == archive || $sub == restore ) && "
        "$COMP_CWORD -eq 3 ]]; then\n"
        "        _dispatch_complete_candidates agents\n"
        "      fi\n"
        "      ;;\n"
        "    workspace)\n"
        "      if (( COMP_CWORD == 2 )); then\n"
        "        _dispatch_complete_words \"create list show remove prune\"\n"
        "      elif [[ $sub == create && $COMP_CWORD -eq 3 ]]; then\n"
        "        _dispatch_complete_candidates tasks\n"
        "      elif [[ ( $sub == show || $sub == remove ) && "
        "$COMP_CWORD -eq 3 ]]; then\n"
        "        _dispatch_complete_candidates workspaces\n"
        "      fi\n"
        "      ;;\n"
        "  esac\n"
        "}\n"
        "\n"
        "complete -F _dispatch_complete dispatch\n",
        stdout);
    return 0;
}

static int cmd_completion_fish(int argc, char **argv) {
    (void)argv;
    if (argc != 3) {
        print_completion_usage();
        return 1;
    }

    fputs(
        "function __dispatch_command\n"
        "    set -l tokens (commandline -opc)\n"
        "    if test (count $tokens) -gt 0\n"
        "        echo $tokens[1]\n"
        "    else\n"
        "        echo dispatch\n"
        "    end\n"
        "end\n"
        "\n"
        "function __dispatch_candidates\n"
        "    set -l cmd (__dispatch_command)\n"
        "    command $cmd completion candidates $argv[1] 2>/dev/null\n"
        "end\n"
        "\n"
        "complete -c dispatch -f\n"
        "complete -c dispatch -f -n 'not __fish_seen_subcommand_from "
        "(__dispatch_candidates commands)' -a '(__dispatch_candidates commands)'\n"
        "\n"
        "complete -c dispatch -f -n '__fish_seen_subcommand_from completion; "
        "and not __fish_seen_subcommand_from candidates bash fish zsh install' -a "
        "'candidates bash fish zsh install'\n"
        "complete -c dispatch -f -n '__fish_seen_subcommand_from completion; "
        "and __fish_seen_subcommand_from candidates' -a '(__dispatch_candidates "
        "candidate-kinds)'\n"
        "complete -c dispatch -f -n '__fish_seen_subcommand_from completion; "
        "and __fish_seen_subcommand_from install' -a 'fish bash zsh'\n"
        "\n"
        "complete -c dispatch -f -n '__fish_seen_subcommand_from show start "
        "finish review ready' -a '(__dispatch_candidates tasks)'\n"
        "complete -c dispatch -f -n '__fish_seen_subcommand_from list' -a "
        "'all (__dispatch_candidates groups)'\n"
        "complete -c dispatch -f -n '__fish_seen_subcommand_from list; and "
        "__fish_seen_subcommand_from all' -a '(__dispatch_candidates groups)'\n"
        "\n"
        "complete -c dispatch -f -n '__fish_seen_subcommand_from group; and "
        "not __fish_seen_subcommand_from add ready' -a 'add ready'\n"
        "complete -c dispatch -f -n '__fish_seen_subcommand_from group; and "
        "__fish_seen_subcommand_from ready' -a '(__dispatch_candidates groups)'\n"
        "\n"
        "complete -c dispatch -f -n '__fish_seen_subcommand_from task; and "
        "not __fish_seen_subcommand_from add delete' -a 'add delete'\n"
        "complete -c dispatch -f -n '__fish_seen_subcommand_from task; and "
        "__fish_seen_subcommand_from delete' -a '(__dispatch_candidates tasks)'\n"
        "\n"
        "complete -c dispatch -f -n '__fish_seen_subcommand_from dep; and not "
        "__fish_seen_subcommand_from add remove' -a 'add remove'\n"
        "complete -c dispatch -f -n '__fish_seen_subcommand_from dep; and "
        "__fish_seen_subcommand_from add remove' -a '(__dispatch_candidates tasks)'\n"
        "\n"
        "complete -c dispatch -f -n '__fish_seen_subcommand_from commit; and not "
        "__fish_seen_subcommand_from add list show' -a 'add list show'\n"
        "complete -c dispatch -f -n '__fish_seen_subcommand_from commit; and "
        "__fish_seen_subcommand_from add list show' -a '(__dispatch_candidates tasks)'\n"
        "\n"
        "complete -c dispatch -f -n '__fish_seen_subcommand_from agent; and not "
        "__fish_seen_subcommand_from create list show command session resume archive restore' -a "
        "'create list show command session resume archive restore'\n"
        "complete -c dispatch -f -n '__fish_seen_subcommand_from agent; and "
        "__fish_seen_subcommand_from show command session resume archive restore' -a "
        "'(__dispatch_candidates agents)'\n"
        "\n"
        "complete -c dispatch -f -n '__fish_seen_subcommand_from workspace; and "
        "not __fish_seen_subcommand_from create list show remove prune' -a "
        "'create list show remove prune'\n"
        "complete -c dispatch -f -n '__fish_seen_subcommand_from workspace; and "
        "__fish_seen_subcommand_from create' -a '(__dispatch_candidates tasks)'\n"
        "complete -c dispatch -f -n '__fish_seen_subcommand_from workspace; and "
        "__fish_seen_subcommand_from show remove' -a '(__dispatch_candidates "
        "workspaces)'\n"
        "\n"
        "complete -c dispatch -f -n '__fish_seen_subcommand_from ready' -l actor\n"
        "complete -c dispatch -f -n '__fish_seen_subcommand_from ready' -l "
        "no-review\n"
        "complete -c dispatch -f -n '__fish_seen_subcommand_from start finish "
        "review' -l actor\n"
        "complete -c dispatch -f -n '__fish_seen_subcommand_from group; and "
        "__fish_seen_subcommand_from ready' -l actor\n"
        "complete -c dispatch -f -n '__fish_seen_subcommand_from group; and "
        "__fish_seen_subcommand_from ready' -l no-review\n"
        "complete -c dispatch -f -n '__fish_seen_subcommand_from task; and "
        "__fish_seen_subcommand_from add' -l description\n"
        "complete -c dispatch -f -n '__fish_seen_subcommand_from task; and "
        "__fish_seen_subcommand_from add' -l actor\n"
        "complete -c dispatch -f -n '__fish_seen_subcommand_from task; and "
        "__fish_seen_subcommand_from add' -l no-review\n"
        "complete -c dispatch -f -n '__fish_seen_subcommand_from task; and "
        "__fish_seen_subcommand_from delete' -l force\n"
        "complete -c dispatch -f -n '__fish_seen_subcommand_from commit; and "
        "__fish_seen_subcommand_from add' -l actor\n"
        "complete -c dispatch -f -n '__fish_seen_subcommand_from workspace; and "
        "__fish_seen_subcommand_from create' -l actor\n"
        "complete -c dispatch -f -n '__fish_seen_subcommand_from workspace; and "
        "__fish_seen_subcommand_from create' -l repo\n"
        "complete -c dispatch -f -n '__fish_seen_subcommand_from workspace; and "
        "__fish_seen_subcommand_from create' -l dir\n"
        "complete -c dispatch -f -n '__fish_seen_subcommand_from workspace; and "
        "__fish_seen_subcommand_from create' -l branch\n"
        "complete -c dispatch -f -n '__fish_seen_subcommand_from workspace; and "
        "__fish_seen_subcommand_from create' -l sequence\n"
        "complete -c dispatch -f -n '__fish_seen_subcommand_from workspace; and "
        "__fish_seen_subcommand_from remove' -l force\n"
        "complete -c dispatch -f -n '__fish_seen_subcommand_from workspace; and "
        "__fish_seen_subcommand_from prune' -l done\n"
        "complete -c dispatch -f -n '__fish_seen_subcommand_from workspace; and "
        "__fish_seen_subcommand_from prune' -l stale\n"
        "complete -c dispatch -f -n '__fish_seen_subcommand_from workspace; and "
        "__fish_seen_subcommand_from prune' -l dry-run\n"
        "complete -c dispatch -f -n '__fish_seen_subcommand_from agent; and "
        "__fish_seen_subcommand_from create' -l name\n"
        "complete -c dispatch -f -n '__fish_seen_subcommand_from agent; and "
        "__fish_seen_subcommand_from create' -l runner\n"
        "complete -c dispatch -f -n '__fish_seen_subcommand_from agent; and "
        "__fish_seen_subcommand_from create' -l model\n"
        "complete -c dispatch -f -n '__fish_seen_subcommand_from agent; and "
        "__fish_seen_subcommand_from create' -l no-run-script\n"
        "complete -c dispatch -f -n '__fish_seen_subcommand_from agent; and "
        "__fish_seen_subcommand_from create' -l print-command\n"
        "complete -c dispatch -f -n '__fish_seen_subcommand_from agent; and "
        "__fish_seen_subcommand_from list' -l all\n"
        "complete -c dispatch -f -n '__fish_seen_subcommand_from agent; and "
        "__fish_seen_subcommand_from command resume' -l print-command\n",
        stdout);
    return 0;
}

static int make_dir_recursive(const char *path) {
    if (!path || path[0] == '\0')
        return 0;

    char *copy = cli_strdup(path);
    for (char *cursor = copy + 1; *cursor; cursor++) {
        if (*cursor != '/')
            continue;
        *cursor = '\0';
        if (!make_dir_if_needed(copy)) {
            free(copy);
            return 0;
        }
        *cursor = '/';
    }

    int ok = make_dir_if_needed(copy);
    free(copy);
    return ok;
}

static char *path_parent_dir(const char *path) {
    const char *slash = strrchr(path, '/');
    if (!slash)
        return cli_strdup(".");
    size_t len = (size_t)(slash - path);
    if (len == 0)
        len = 1;
    char *dir = malloc(len + 1);
    if (!dir) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    memcpy(dir, path, len);
    dir[len] = '\0';
    return dir;
}

int command_exists_on_path(const char *command) {
    const char *path_env = getenv("PATH");
    if (!path_env || !path_env[0])
        return 0;

    char *paths = cli_strdup(path_env);
    char *saveptr = NULL;
    for (char *dir = strtok_r(paths, ":", &saveptr); dir;
         dir = strtok_r(NULL, ":", &saveptr)) {
        char *candidate = join_path2(dir[0] ? dir : ".", command);
        int found = access(candidate, X_OK) == 0;
        free(candidate);
        if (found) {
            free(paths);
            return 1;
        }
    }

    free(paths);
    return 0;
}

char *completion_install_path(const char *shell_name) {
    const char *home = getenv("HOME");
    if (!home || !home[0])
        return NULL;

    if (strcmp(shell_name, "fish") == 0) {
        const char *xdg_config = getenv("XDG_CONFIG_HOME");
        char *base = xdg_config && xdg_config[0]
                         ? cli_strdup(xdg_config)
                         : join_path2(home, ".config");
        char *dir = join_path2(base, "fish/completions");
        char *path = join_path2(dir, "dispatch.fish");
        free(base);
        free(dir);
        return path;
    }

    const char *xdg_data = getenv("XDG_DATA_HOME");
    char *data_base = xdg_data && xdg_data[0]
                          ? cli_strdup(xdg_data)
                          : join_path2(home, ".local/share");
    char *path = NULL;
    if (strcmp(shell_name, "bash") == 0) {
        char *dir = join_path2(data_base, "bash-completion/completions");
        path = join_path2(dir, "dispatch");
        free(dir);
    } else if (strcmp(shell_name, "zsh") == 0) {
        char *dir = join_path2(data_base, "zsh/site-functions");
        path = join_path2(dir, "_dispatch");
        free(dir);
    }
    free(data_base);
    return path;
}

static const char *completion_reload_hint(const char *shell_name,
                                          const char *path) {
    (void)path;
    if (strcmp(shell_name, "fish") == 0)
        return "Run: exec fish";
    if (strcmp(shell_name, "bash") == 0)
        return "Run: source ~/.bashrc, or start a new bash shell";
    return "Ensure ~/.local/share/zsh/site-functions is in fpath, then run: autoload -Uz compinit; compinit";
}

static int write_completion_script_file(const char *shell_name,
                                        const char *path) {
    char *dir = path_parent_dir(path);
    if (!make_dir_recursive(dir)) {
        fprintf(stderr, "Could not create completion directory %s: %s\n", dir,
                strerror(errno));
        free(dir);
        return 0;
    }
    free(dir);

    FILE *file = fopen(path, "w");
    if (!file) {
        fprintf(stderr, "Could not write %s: %s\n", path, strerror(errno));
        return 0;
    }

    fflush(stdout);
    int saved_stdout = dup(STDOUT_FILENO);
    if (saved_stdout < 0 || dup2(fileno(file), STDOUT_FILENO) < 0) {
        if (saved_stdout >= 0)
            close(saved_stdout);
        fclose(file);
        fprintf(stderr, "Could not redirect completion output: %s\n",
                strerror(errno));
        return 0;
    }

    char *argv[] = {"dispatch", "completion", (char *)shell_name, NULL};
    int result = 1;
    if (strcmp(shell_name, "fish") == 0)
        result = cmd_completion_fish(3, argv);
    else if (strcmp(shell_name, "bash") == 0)
        result = cmd_completion_bash(3, argv);
    else if (strcmp(shell_name, "zsh") == 0)
        result = cmd_completion_zsh(3, argv);

    fflush(stdout);
    dup2(saved_stdout, STDOUT_FILENO);
    close(saved_stdout);
    fclose(file);
    return result == 0;
}

static int cmd_completion_install(int argc, char **argv) {
    if (argc != 4) {
        print_completion_usage();
        return 1;
    }

    const char *shell_name = argv[3];
    if (strcmp(shell_name, "fish") != 0 && strcmp(shell_name, "bash") != 0 &&
        strcmp(shell_name, "zsh") != 0) {
        print_completion_usage();
        return 1;
    }

    char *path = completion_install_path(shell_name);
    if (!path) {
        fprintf(stderr, "HOME must be set to install completions\n");
        return 1;
    }

    if (!write_completion_script_file(shell_name, path)) {
        free(path);
        return 1;
    }

    printf("Installed %s completion: %s\n", shell_name, path);
    if (!command_exists_on_path("dispatch")) {
        printf("Warning: dispatch was not found on PATH; completions may not load dynamic candidates.\n");
    }
    printf("%s\n", completion_reload_hint(shell_name, path));
    free(path);
    return 0;
}

int cmd_completion(int argc, char **argv) {
    if (argc >= 3 && strcmp(argv[2], "candidates") == 0)
        return cmd_completion_candidates(argc, argv);
    if (argc >= 3 && strcmp(argv[2], "bash") == 0)
        return cmd_completion_bash(argc, argv);
    if (argc >= 3 && strcmp(argv[2], "fish") == 0)
        return cmd_completion_fish(argc, argv);
    if (argc >= 3 && strcmp(argv[2], "zsh") == 0)
        return cmd_completion_zsh(argc, argv);
    if (argc >= 3 && strcmp(argv[2], "install") == 0)
        return cmd_completion_install(argc, argv);

    print_completion_usage();
    return 1;
}
