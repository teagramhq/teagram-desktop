#!/usr/bin/env bash
set -euo pipefail

scale_script=.github/scripts/desktop_scale_check.py
history_file="$RUNNER_TEMP/desktop-scale-history.json"
summary_file="$RUNNER_TEMP/desktop-scale-summary.md"
evaluation_file="$RUNNER_TEMP/desktop-scale-evaluation.json"
app_mount="$RUNNER_TEMP/desktop-scale-dev"
scale_budget_seconds="${DESKTOP_SCALE_RUN_BUDGET_SECONDS:-180}"
# A hosted upload of the 236 MB Teagram artifact took 27 seconds.
# Reserve 50 seconds for the base-run lookup, download, and extraction.
# The history, binary fetch, and scale caps total 285 seconds.
binary_fetch_budget_seconds=50
scale_history_args=()
scale_baseline_count=0
scale_history_truncated=false
scale_history_unavailable=false
scale_binary_available=false
scale_history_malformed_count=0
scale_history_first_error=
printf '[]\n' > "$history_file"

if [[ "$GITHUB_EVENT_NAME" == 'pull_request' \
	|| "$GITHUB_REF" == 'refs/heads/dev' ]]; then
	fetch_output="$RUNNER_TEMP/desktop-scale-fetch-output.txt"
	set +e
	timeout 55s python3 "$scale_script" fetch \
		--history-only \
		--api-url "$GITHUB_API_URL" \
		--token "$GH_TOKEN" \
		--repository "$GITHUB_REPOSITORY" \
		--records-output "$history_file" > "$fetch_output"
	history_fetch_status=$?
	set -e
	if (( history_fetch_status == 0 )); then
		scale_baseline_count="$(sed -n 's/^record_count=//p' "$fetch_output")"
		if [[ ! "$scale_baseline_count" =~ ^[0-9]+$ ]]; then
			echo '::error::Desktop scale history fetch returned an invalid record count.'
			exit 2
		fi
		scale_history_truncated="$(sed -n 's/^history_truncated=//p' "$fetch_output")"
		scale_history_malformed_count="$(sed -n 's/^history_malformed_count=//p' "$fetch_output")"
		scale_history_first_error="$(sed -n 's/^history_first_error=//p' "$fetch_output")"
		if [[ ! "$scale_history_malformed_count" =~ ^[0-9]+$ ]]; then
			echo '::error::Desktop scale history fetch returned an invalid malformed-record count.'
			exit 2
		fi
		scale_history_args+=(--history-malformed-count "$scale_history_malformed_count")
		scale_history_args+=(--history-first-error "$scale_history_first_error")
		if [[ "$scale_history_truncated" == 'true' ]]; then
			scale_history_args+=(--history-truncated)
		fi
		if [[ "$GITHUB_EVENT_NAME" == 'pull_request' \
			&& "$scale_baseline_count" -ge 20 ]]; then
			binary_fetch_started=$SECONDS
			set +e
			timeout "${binary_fetch_budget_seconds}s" python3 "$scale_script" fetch-binary \
				--api-url "$GITHUB_API_URL" \
				--token "$GH_TOKEN" \
				--repository "$GITHUB_REPOSITORY" \
				--base-sha "$PR_BASE_SHA" \
				--binary-directory "$app_mount"
			binary_fetch_status=$?
			set -e
			if (( binary_fetch_status != 0 )); then
				if (( binary_fetch_status == 124 )); then
					printf '::error::PR base Teagram artifact fetch timed out after the '
					printf '%s-second budget; re-run this job.\n' "$binary_fetch_budget_seconds"
				else
					echo "::error::PR base Teagram artifact fetch failed with status $binary_fetch_status after dev history was available."
				fi
				cat >> "$GITHUB_STEP_SUMMARY" <<EOF
### Desktop data scale check

No timing verdict was produced because the PR base binary fetch failed with status $binary_fetch_status after $scale_baseline_count dev samples were available.
EOF
				exit "$binary_fetch_status"
			fi
			binary_fetch_elapsed=$((SECONDS - binary_fetch_started))
			echo "::notice::PR base Teagram artifact retrieved in ${binary_fetch_elapsed}s."
			scale_binary_available=true
		fi
	else
		printf '[]\n' > "$history_file"
		scale_baseline_count=0
		scale_history_unavailable=true
		scale_history_malformed_count=0
		scale_history_first_error=
		if (( history_fetch_status == 124 )); then
			echo '::error::Desktop scale history fetch timed out after 55 seconds; this run will stay report-only.'
			echo '::warning::Desktop scale baseline history is unavailable for this run.'
		else
			echo "::warning::Could not read prior desktop scale history (status $history_fetch_status); this run will stay report-only."
		fi
	fi
fi

if [[ "$scale_history_unavailable" == 'true' ]]; then
	scale_history_args+=(--history-unavailable)
fi
printf 'record_count=%s\nbinary_available=%s\nhistory_truncated=%s\nhistory_unavailable=%s\n' \
	"$scale_baseline_count" "$scale_binary_available" \
	"$scale_history_truncated" "$scale_history_unavailable" >> "$GITHUB_OUTPUT"
scale_started=$SECONDS

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
	if (( run_status == 0 )) || grep -Fqx DESKTOP_SCALE_REPORT_BEGIN "$log_file"; then
		if python3 "$scale_script" parse \
			--report "$log_file" \
			--output "$sample_file" \
			--run-id "$GITHUB_RUN_ID" \
			--run-attempt "$GITHUB_RUN_ATTEMPT" \
			--commit "$GITHUB_SHA"; then
			:
		else
			local parse_status=$?
			if (( run_status == 0 )); then
				return "$parse_status"
			fi
			echo "::warning::Could not parse the report from scale run $label (status $parse_status); preserving app status $run_status."
		fi
	fi
	if (( run_status != 0 )); then
		echo "::error::Desktop scale run $label exited with status $run_status."
		return "$run_status"
	fi
	return 0
}

append_summary() {
	local failed_step="${1:-}"
	local failed_status="${2:-}"
	if [[ -s "$summary_file" ]]; then
		cat "$summary_file" >> "$GITHUB_STEP_SUMMARY"
		if [[ -n "$failed_step" ]]; then
			printf '\nNo final timing verdict was produced because %s failed with status %s.\n' \
				"$failed_step" "$failed_status" >> "$GITHUB_STEP_SUMMARY"
		fi
	elif [[ -n "$failed_step" ]]; then
		cat >> "$GITHUB_STEP_SUMMARY" <<EOF
### Desktop data scale check

No timing verdict was produced because $failed_step failed with status $failed_status.
EOF
	else
		cat >> "$GITHUB_STEP_SUMMARY" <<EOF
### Desktop data scale check

No timing verdict was produced because no scale-check summary was generated.
EOF
	fi
	if [[ -s "$evaluation_file" ]]; then
		python3 "$scale_script" annotate --evaluation "$evaluation_file"
	fi
}

record_run_failure() {
	local label="$1"
	local status="$2"
	append_summary "scale run $label" "$status"
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
	: > "$evaluation_file"
	: > "$summary_file"
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
		if (( evaluation_status == 0 )); then
			append_summary
		else
			append_summary 'dev evaluation' "$evaluation_status"
		fi
		cp "$sample_file" "$RUNNER_TEMP/desktop-scale-sample.json"
		echo 'recorded=true' >> "$GITHUB_OUTPUT"
	else
		append_summary 'scale run head-1' "${run_status:-2}"
	fi
	if (( run_status != 0 )); then
		exit "$run_status"
	fi
	exit "${evaluation_status:-2}"
fi

if [[ "$GITHUB_EVENT_NAME" == 'pull_request' \
	&& "$scale_baseline_count" -ge 20 ]]; then
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
	evaluation_step='initial evaluation'
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
		evaluation_step='retry evaluation'
	fi
	if (( evaluation_status == 0 || evaluation_status == 1 )); then
		append_summary
	else
		append_summary "$evaluation_step" "$evaluation_status"
	fi
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
	append_summary 'report-only evaluation' "$status"
	exit "$status"
fi
append_summary
