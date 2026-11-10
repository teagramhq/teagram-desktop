#!/usr/bin/env bash
set -euo pipefail

scale_script=.github/scripts/desktop_scale_check.py
history_file="$RUNNER_TEMP/desktop-scale-history.json"
summary_file="$RUNNER_TEMP/desktop-scale-summary.md"
evaluation_file="$RUNNER_TEMP/desktop-scale-evaluation.json"
app_mount="$RUNNER_TEMP/desktop-scale-dev"
scale_started=$SECONDS
scale_budget_seconds="${DESKTOP_SCALE_RUN_BUDGET_SECONDS:-180}"
scale_baseline_count="${SCALE_BASELINE_COUNT:-0}"
scale_history_args=()

if [[ "${SCALE_BASELINE_TRUNCATED:-false}" == 'true' ]]; then
	scale_history_args+=(--history-truncated)
fi
if [[ "${SCALE_BASELINE_UNAVAILABLE:-false}" == 'true' ]]; then
	scale_history_args+=(--history-unavailable)
fi

run_scale_check() {
	local label="$1"
	local source="$2"
	local log_file="$RUNNER_TEMP/desktop-scale-$label.log"
	local sample_file="$RUNNER_TEMP/desktop-scale-$label.json"
	local binary_path=/usr/src/tdesktop/out/Debug/Teagram
	local binary_mount=()
	if [[ "$source" == 'base' ]]; then
		binary_path=/desktop-scale/Teagram
		binary_mount=(-v "$app_mount:/desktop-scale:ro")
	fi
	local remaining=$((scale_budget_seconds - (SECONDS - scale_started)))
	if (( remaining <= 0 )); then
		echo "::error::Desktop scale run $label timed out because the $scale_budget_seconds-second scale-check budget was exhausted."
		return 124
	fi
	set +e
	timeout "${remaining}s" docker run --rm \
		-u "$(id -u)" \
		-v "$PWD:/usr/src/tdesktop" \
		"${binary_mount[@]}" \
		-e DESKTOP_SCALE_BINARY="$binary_path" \
		"$IMAGE_TAG" \
		bash -lc '
			cd /usr/src/tdesktop
			scale_home=$(mktemp -d /tmp/tdesktop-scale-check.XXXXXX)
			mkdir -p "$scale_home/config" "$scale_home/data" "$scale_home/cache" "$scale_home/work"
			export HOME="$scale_home"
			export XDG_CONFIG_HOME="$scale_home/config"
			export XDG_DATA_HOME="$scale_home/data"
			export XDG_CACHE_HOME="$scale_home/cache"
			export QT_QPA_PLATFORM=offscreen
			export ALSOFT_DRIVERS=null
			export TDESKTOP_DESKTOP_SCALE_CHECK=1
			export TDESKTOP_UI_STALL_WATCHDOG_CACHE_REGRESSION=1
			export LD_PRELOAD=/usr/src/tdesktop/out/Debug/libdesktop_scale_check_fs_counter.so
			app_output="$scale_home/app-output.txt"
			"$DESKTOP_SCALE_BINARY" -noupdate -debug \
				-workdir "$scale_home/work/" > "$app_output" 2>&1
			app_status=$?
			cat "$app_output"
			if ! grep -Fqx DESKTOP_SCALE_REPORT_END "$app_output"; then
				echo "::group::Scale check exited with status $app_status without a report"
				find "$scale_home/work/DebugLogs" -type f -name "log_*.txt" \
					-print -exec tail -n 30 {} \;
				echo "::endgroup::"
				if test "$app_status" -ge 128; then
					echo "::group::Backtrace of the crashed scale check"
					unset LD_PRELOAD
					gdb -batch -ex "set environment LD_PRELOAD /usr/src/tdesktop/out/Debug/libdesktop_scale_check_fs_counter.so" \
						-ex "set debuginfod enabled off" \
						-ex "handle SIGPIPE nostop noprint pass" \
						-ex "handle SIGUSR2 nostop noprint pass" \
						-ex run -ex "bt 40" -ex "thread apply all bt 12" \
						--args "$DESKTOP_SCALE_BINARY" -noupdate -debug \
						-workdir "$scale_home/work2/" 2>&1 | grep -v "^Downloading separate\|^\[New Thread\|^\[Detaching" | tail -n 150
					echo "::endgroup::"
				fi
			fi
			exit "$app_status"
		' | tee "$log_file"
	local statuses=("${PIPESTATUS[@]}")
	local run_status=${statuses[0]}
	local tee_status=${statuses[1]}
	set -e
	if (( tee_status != 0 )); then
		return "$tee_status"
	fi
	if (( run_status == 124 )); then
		echo "::error::Desktop scale run $label timed out after exhausting the remaining ${remaining}-second scale-check budget (exit 124)."
		return 124
	fi
	if python3 "$scale_script" parse \
		--report "$log_file" \
		--output "$sample_file" \
		--run-id "$GITHUB_RUN_ID" \
		--run-attempt "$GITHUB_RUN_ATTEMPT" \
		--commit "$GITHUB_SHA"; then
		:
	else
		local parse_status=$?
		return "$parse_status"
	fi
	return "$run_status"
}

append_summary() {
	if [[ -s "$summary_file" ]]; then
		cat "$summary_file" >> "$GITHUB_STEP_SUMMARY"
	else
		cat >> "$GITHUB_STEP_SUMMARY" <<EOF
### Desktop data scale check

No timing verdict was produced because scale run ${SCALE_FAILED_RUN:-unknown} failed with status ${SCALE_FAILED_STATUS:-unknown}.
EOF
	fi
	if [[ -s "$evaluation_file" ]]; then
		python3 "$scale_script" annotate --evaluation "$evaluation_file"
	fi
}

record_run_failure() {
	local label="$1"
	local status="$2"
	SCALE_FAILED_RUN="$label"
	SCALE_FAILED_STATUS="$status"
	if [[ -s "$summary_file" ]]; then
		printf '\nTiming comparison did not complete: scale run %s failed with status %s.\n' \
			"$label" "$status" >> "$summary_file"
	fi
	append_summary
	echo "::error::Desktop scale check run $label failed with status $status."
	return "$status"
}

run_and_require_success() {
	local label="$1"
	local source="$2"
	if run_scale_check "$label" "$source"; then
		return 0
	else
		local status=$?
		record_run_failure "$label" "$status"
		return "$status"
	fi
}

run_interleaved_pair() {
	local pair="$1"
	if run_and_require_success "base-$pair" base; then
		:
	else
		return $?
	fi
	if run_and_require_success "head-$pair" head; then
		:
	else
		return $?
	fi
}

evaluate_samples() {
	local retry_note="${1:-}"
	local report_only="${2:-false}"
	local absolute_budget_head_sample="${3:-}"
	local -a args=(
		--history "$history_file"
		--output "$evaluation_file"
		--summary-output "$summary_file"
		"${scale_history_args[@]}"
	)
	for path in "${base_sample_paths[@]}"; do
		args+=(--base "$path")
	done
	for path in "${head_sample_paths[@]}"; do
		args+=(--head "$path")
	done
	if [[ -n "$absolute_budget_head_sample" ]]; then
		args+=(--absolute-budget-head "$absolute_budget_head_sample")
	fi
	if [[ -n "$retry_note" ]]; then
		args+=(--retry-note "$retry_note")
	fi
	if [[ "$report_only" == 'true' ]]; then
		args+=(--report-only)
	fi
	set +e
	python3 "$scale_script" evaluate "${args[@]}"
	local result=$?
	set -e
	return "$result"
}

if [[ "$GITHUB_REF" == 'refs/heads/dev' ]]; then
	run_status=0
	if run_scale_check head-1 head; then
		:
	else
		run_status=$?
	fi
	sample_file="$RUNNER_TEMP/desktop-scale-head-1.json"
	if [[ -s "$sample_file" ]]; then
		base_sample_paths=("$sample_file")
		head_sample_paths=("$sample_file")
		set +e
		python3 "$scale_script" evaluate \
			--history "$history_file" \
			--base "$sample_file" \
			--head "$sample_file" \
			--output "$evaluation_file" \
			--summary-output "$summary_file" \
			--report-only \
			--include-head-in-history \
			"${scale_history_args[@]}"
		evaluation_status=$?
		set -e
		append_summary
		cp "$sample_file" "$RUNNER_TEMP/desktop-scale-sample.json"
		echo 'recorded=true' >> "$GITHUB_OUTPUT"
	else
		SCALE_FAILED_RUN=head-1
		SCALE_FAILED_STATUS=${run_status:-2}
		append_summary
	fi
	if (( run_status != 0 )); then
		exit "$run_status"
	fi
	exit "${evaluation_status:-2}"
fi

if [[ "$GITHUB_EVENT_NAME" == 'pull_request' \
	&& "$scale_baseline_count" -ge 20 ]]; then
	if [[ "${SCALE_BINARY_AVAILABLE:-false}" != 'true' ]]; then
		echo "::error::No Teagram artifact is available for PR base SHA ${PR_BASE_SHA:-unknown}. Rebase onto an earlier dev commit or rerun after the dev build for this SHA completes."
		exit 2
	fi
	for pair in 1 2; do
		if run_interleaved_pair "$pair"; then
			:
		else
			status=$?
			exit "$status"
		fi
	done
	base_sample_paths=(
		"$RUNNER_TEMP/desktop-scale-base-1.json"
		"$RUNNER_TEMP/desktop-scale-base-2.json"
	)
	head_sample_paths=(
		"$RUNNER_TEMP/desktop-scale-head-1.json"
		"$RUNNER_TEMP/desktop-scale-head-2.json"
	)
	if evaluate_samples; then
		evaluation_status=0
	else
		evaluation_status=$?
	fi
	if (( evaluation_status == 1 )); then
		echo 'Desktop scale timing threshold crossed; adding one interleaved sample pair before judging the result.'
		if run_interleaved_pair 3; then
			:
		else
			status=$?
			exit "$status"
		fi
		base_sample_paths+=("$RUNNER_TEMP/desktop-scale-base-3.json")
		head_sample_paths+=("$RUNNER_TEMP/desktop-scale-head-3.json")
		retry_note='The initial two-pair comparison crossed a timing limit. One additional interleaved pair was added, and the result uses all three pairs for timing medians; the absolute budget is checked on the retry head sample.'
		if evaluate_samples \
			"$retry_note" false "$RUNNER_TEMP/desktop-scale-head-3.json"; then
			evaluation_status=0
		else
			evaluation_status=$?
		fi
	fi
	append_summary
	exit "$evaluation_status"
fi

if run_scale_check head-1 head; then
	:
else
	status=$?
	record_run_failure head-1 "$status"
	exit "$status"
fi
base_sample_paths=()
head_sample_paths=("$RUNNER_TEMP/desktop-scale-head-1.json")
if evaluate_samples "" true; then
	:
else
	status=$?
	append_summary
	exit "$status"
fi
append_summary
