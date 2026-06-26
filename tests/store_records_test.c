#include "dispatch_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fail(const char *message, const char *detail) {
    if (detail && detail[0])
        fprintf(stderr, "FAIL: %s: %s\n", message, detail);
    else
        fprintf(stderr, "FAIL: %s\n", message);
    return 1;
}

static char *copy_string(const char *value) {
    char *copy = strdup(value ? value : "");
    if (!copy) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    return copy;
}

static int assert_string(const char *actual, const char *expected,
                         const char *label) {
    if (!actual || strcmp(actual, expected) != 0)
        return fail(label, actual ? actual : "(null)");
    return 0;
}

static void add_agent_record(DispatchBoard *board) {
    board->agents.items = calloc(1, sizeof(*board->agents.items));
    if (!board->agents.items) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    board->agents.count = 1;
    board->agents.capacity = 1;

    DispatchAgent *agent = &board->agents.items[0];
    agent->name = copy_string("codex-server");
    agent->runner = copy_string("codex");
    agent->model = copy_string("gpt-5.3-codex");
    agent->agent_dir = copy_string(".dispatch/agents/codex-server");
    agent->prompt_path =
        copy_string(".dispatch/agents/codex-server/codex-server-PROMPT.md");
    agent->run_script_path = copy_string(".dispatch/agents/codex-server/run.sh");
    agent->session_id = copy_string("codex-session-1");
    agent->current_task = copy_string("DE-01");
    agent->last_workspace = copy_string("DE-01");
    agent->created_at = 11;
    agent->updated_at = 44;
}

static void add_workspace_record(DispatchBoard *board) {
    board->workspaces.items = calloc(1, sizeof(*board->workspaces.items));
    if (!board->workspaces.items) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    board->workspaces.count = 1;
    board->workspaces.capacity = 1;

    DispatchWorkspace *workspace = &board->workspaces.items[0];
    workspace->id = copy_string("WS-01");
    workspace->task_id = copy_string("DE-01");
    workspace->actor = copy_string("codex-server");
    workspace->path = copy_string("/tmp/Dispatch-agent-codex-server-DE-01");
    workspace->branch = copy_string("agent/codex-server/DE-01");
    workspace->repo_path = copy_string("repo");
    workspace->state = DISPATCH_WORKSPACE_ACTIVE;
    workspace->sequence_tasks.items =
        calloc(1, sizeof(*workspace->sequence_tasks.items));
    if (!workspace->sequence_tasks.items) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    workspace->sequence_tasks.items[0] = copy_string("DE-01");
    workspace->sequence_tasks.count = 1;
    workspace->sequence_tasks.capacity = 1;
    workspace->review_gate = copy_string("DE-02");
    workspace->created_at = 22;
    workspace->updated_at = 33;
}

static int verify_empty_round_trip(const char *path) {
    char error[256] = {0};
    DispatchBoard board;
    dispatch_board_init(&board, "Dispatch");
    dispatch_board_set_repo_path(&board, "repo");

    if (!dispatch_store_save(&board, path, error, sizeof(error))) {
        dispatch_board_free(&board);
        return fail("empty save failed", error);
    }
    dispatch_board_free(&board);

    DispatchBoard loaded;
    if (!dispatch_store_load(&loaded, path, error, sizeof(error)))
        return fail("empty load failed", error);

    if (loaded.agents.count != 0 || loaded.workspaces.count != 0) {
        dispatch_board_free(&loaded);
        return fail("empty records were not empty", "");
    }
    dispatch_board_free(&loaded);
    return 0;
}

static int verify_populated_round_trip(const char *path) {
    char error[256] = {0};
    DispatchBoard board;
    dispatch_board_init(&board, "Dispatch");
    dispatch_board_set_repo_path(&board, "repo");
    dispatch_board_add_group(&board, "Development", "DE");
    dispatch_board_add_task(&board, "DE", "Root", "");
    add_agent_record(&board);
    add_workspace_record(&board);

    if (!dispatch_store_save(&board, path, error, sizeof(error))) {
        dispatch_board_free(&board);
        return fail("populated save failed", error);
    }
    dispatch_board_free(&board);

    DispatchBoard loaded;
    if (!dispatch_store_load(&loaded, path, error, sizeof(error)))
        return fail("populated load failed", error);

    if (loaded.agents.count != 1 || loaded.workspaces.count != 1) {
        dispatch_board_free(&loaded);
        return fail("record counts did not round trip", "");
    }

    DispatchAgent *agent = dispatch_board_find_agent(&loaded, "codex-server");
    DispatchWorkspace *workspace =
        dispatch_board_find_workspace(&loaded, "DE-01");
    if (!agent || !workspace) {
        dispatch_board_free(&loaded);
        return fail("loaded records were not findable", "");
    }

    if (assert_string(agent->runner, "codex", "agent runner") ||
        assert_string(agent->model, "gpt-5.3-codex", "agent model") ||
        assert_string(agent->session_id, "codex-session-1",
                      "agent session id") ||
        assert_string(agent->current_task, "DE-01", "agent current task") ||
        assert_string(agent->last_workspace, "DE-01",
                      "agent last workspace") ||
        assert_string(workspace->actor, "codex-server", "workspace actor") ||
        assert_string(workspace->branch, "agent/codex-server/DE-01",
                      "workspace branch") ||
        assert_string(dispatch_workspace_state_name(workspace->state),
                      "active", "workspace state")) {
        dispatch_board_free(&loaded);
        return 1;
    }

    if (workspace->sequence_tasks.count != 1 ||
        strcmp(workspace->sequence_tasks.items[0], "DE-01") != 0) {
        dispatch_board_free(&loaded);
        return fail("workspace sequence did not round trip", "");
    }

    free(agent->session_id);
    agent->session_id = copy_string("  codex-session-1  \t");
    if (dispatch_board_normalize_agent_sessions(&loaded) != 1 ||
        assert_string(agent->session_id, "codex-session-1",
                      "trimmed agent session id")) {
        dispatch_board_free(&loaded);
        return 1;
    }
    free(agent->session_id);
    agent->session_id = copy_string("  \t");
    if (dispatch_board_normalize_agent_sessions(&loaded) != 1 ||
        agent->session_id != NULL) {
        dispatch_board_free(&loaded);
        return fail("blank agent session id was not cleared", "");
    }
    agent->session_id = copy_string("codex-session-1");

    if (!dispatch_store_save(&loaded, path, error, sizeof(error))) {
        dispatch_board_free(&loaded);
        return fail("resave failed", error);
    }
    dispatch_board_free(&loaded);

    DispatchBoard reloaded;
    if (!dispatch_store_load(&reloaded, path, error, sizeof(error)))
        return fail("reload after resave failed", error);
    int ok = reloaded.agents.count == 1 && reloaded.workspaces.count == 1;
    dispatch_board_free(&reloaded);
    return ok ? 0 : fail("resaved record counts changed", "");
}

int main(int argc, char **argv) {
    if (argc != 3)
        return fail("usage: store_records_test <empty-path> <populated-path>",
                    "");

    if (verify_empty_round_trip(argv[1]))
        return 1;
    if (verify_populated_round_trip(argv[2]))
        return 1;

    printf("PASS: store records\n");
    return 0;
}
