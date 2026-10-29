# $ai_wait must return one boolean. Fail on timeout instead of passing a NULL.
let $ai_wait_attempts = 300;
let $ai_wait_ready = 0;
while ($ai_wait_attempts)
{
  let $ai_wait_ready = `$ai_wait`;
  if ($ai_wait_ready == 1) { let $ai_wait_attempts = 0; }
  if ($ai_wait_ready == 0)
  {
    --sleep 0.1
    dec $ai_wait_attempts;
  }
}
if ($ai_wait_ready == 0) { die AI COLUMN condition timed out; }
