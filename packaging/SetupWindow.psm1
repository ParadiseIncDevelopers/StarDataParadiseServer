function Show-SdpSetupWindow {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][hashtable]$Options,
        [Parameter(Mandatory)][scriptblock]$ResolvePlan,
        [Parameter(Mandatory)][scriptblock]$Apply,
        [switch]$Smoke
    )

    Add-Type -AssemblyName System.Windows.Forms
    Add-Type -AssemblyName System.Drawing
    Import-Module (Join-Path $PSScriptRoot 'InstancePlan.psm1')
    [Windows.Forms.Application]::EnableVisualStyles()
    $manifest = Get-Item -LiteralPath $Options.ManifestPath
    if ($manifest.Length -gt 2MB) { throw 'The setup manifest is too large.' }
    $document = Get-Content -LiteralPath $manifest.FullName -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($document.schema -ne 1) { throw 'Unsupported setup manifest.' }

    $form = New-Object Windows.Forms.Form
    $form.Text = 'StarDataParadise Setup'
    $form.ClientSize = New-Object Drawing.Size(860,620)
    $form.MinimumSize = New-Object Drawing.Size(700,500)
    $form.StartPosition = 'CenterScreen'
    $tabControl = New-Object Windows.Forms.TabControl
    $tabControl.Dock = 'Fill'
    $form.Controls.Add($tabControl)

    function Add-Page([string]$Caption) {
        $page = New-Object Windows.Forms.TabPage
        $page.Text = $Caption
        $page.Padding = New-Object Windows.Forms.Padding(12)
        $tabControl.TabPages.Add($page)
        $page
    }
    function Add-Text([object]$Owner,[string]$Value,[bool]$IsReadOnly) {
        $box = New-Object Windows.Forms.TextBox
        $box.Multiline = $true
        $box.ScrollBars = 'Vertical'
        $box.Dock = 'Fill'
        $box.ReadOnly = $IsReadOnly
        $box.Text = $Value
        $Owner.Controls.Add($box)
        $box
    }
    function Add-Field([object]$Grid,[string]$Caption,[string]$Value) {
        $label = New-Object Windows.Forms.Label
        $label.Text = $Caption
        $label.AutoSize = $true
        $label.Anchor = 'Left'
        $box = New-Object Windows.Forms.TextBox
        $box.Text = $Value
        $box.Width = 580
        $row = $Grid.RowCount
        $Grid.RowCount++
        $Grid.Controls.Add($label,0,$row)
        $Grid.Controls.Add($box,1,$row)
        $box
    }
    function Add-Choice([object]$Owner,[object[]]$Items,[int]$Index) {
        $choice = New-Object Windows.Forms.ComboBox
        $choice.DropDownStyle = 'DropDownList'
        $choice.Width = 155
        $choice.Items.AddRange($Items)
        $choice.SelectedIndex = $Index
        $Owner.Controls.Add($choice)
        $choice
    }
    function Add-Check([object]$Owner,[string]$Caption,[bool]$Selected) {
        $check = New-Object Windows.Forms.CheckBox
        $check.Text = $Caption
        $check.AutoSize = $true
        $check.Checked = $Selected
        $Owner.Controls.Add($check)
        $check
    }

    $welcome = Add-Page 'Welcome and licenses'
    $license = Join-Path (Split-Path -Parent $manifest.FullName) 'THIRD-PARTY.md'
    $licenseText = if (Test-Path -LiteralPath $license) { Get-Content -LiteralPath $license -Raw -Encoding UTF8 } else {
        'See the notices included with each application.'
    }
    [void](Add-Text $welcome ("Select the packages to install offline." + [Environment]::NewLine + [Environment]::NewLine + $licenseText) $true)

    $selection = Add-Page 'Components'
    $selectBar = New-Object Windows.Forms.FlowLayoutPanel
    $selectBar.Dock = 'Top'
    $selectBar.Height = 56
    $operations = @('Install','Modify','Repair','Reconfigure','Uninstall','Recover','Portable')
    $operation = Add-Choice $selectBar $operations ([Math]::Max(0, [array]::IndexOf($operations,$Options.Operation)))
    $presets = [ordered]@{ 'Server Only'='server-only'; 'Workbench Only'='workbench-only'; 'CLI Only'='cli-only';
        'Developer'='developer'; 'Complete'='complete'; 'Custom'='custom' }
    $preset = Add-Choice $selectBar @($presets.Keys) 1
    foreach ($name in $presets.Keys) { if ($presets[$name] -eq $Options.Profile) { $preset.SelectedItem = $name } }
    if ($Options.ComponentId) { $preset.SelectedItem = 'Custom' }
    $development = Add-Check $selectBar 'Use unsigned development packages' ([bool]$Options.AllowDevelopment)
    $development.Visible = $document.channel -eq 'development'
    $components = New-Object Windows.Forms.CheckedListBox
    $components.Dock = 'Fill'
    $components.CheckOnClick = $true
    $ids = @($document.components | ForEach-Object { $_.id })
    foreach ($item in $document.components) {
        $caption = $item.id
        if ($item.dependencies.Count) { $caption += '  (requires ' + (($item.dependencies | ForEach-Object { $_.id }) -join ', ') + ')' }
        [void]$components.Items.Add($caption, $item.id -in $Options.ComponentId)
    }
    $selection.Controls.Add($components)
    $selection.Controls.Add($selectBar)
    $components.Enabled = $preset.SelectedItem -eq 'Custom'
    $preset.Add_SelectedIndexChanged({ $components.Enabled = $preset.SelectedItem -eq 'Custom' })

    $paths = Add-Page 'Paths and instance'
    $grid = New-Object Windows.Forms.TableLayoutPanel
    $grid.Dock = 'Fill'
    $grid.AutoScroll = $true
    $grid.ColumnCount = 2
    $paths.Controls.Add($grid)
    $program = Add-Field $grid 'Program root (optional)' $Options.InstallRoot
    $state = Add-Field $grid 'Setup state directory' $Options.StateDirectory
    $portable = Add-Field $grid 'Portable Workbench target' $Options.PortableDirectory
    $instanceDefs = [ordered]@{
        instanceRoot=@('Server instance root (optional)',''); instanceName=@('Instance name','default')
        bind=@('Bind address','127.0.0.1'); port=@('TCP port','7632')
        dataDirectory=@('Data, WAL and model directory',''); logDirectory=@('Log directory','')
        tempDirectory=@('Temporary directory',''); backupDirectory=@('Backup directory','')
        tlsCertificate=@('TLS certificate path',''); tlsPrivateKey=@('TLS key path',''); tlsCa=@('TLS CA certificate path','')
    }
    $fields = @{}
    foreach ($id in $instanceDefs.Keys) {
        $value = if ($Options.Instance -and $Options.Instance[$id]) { $Options.Instance[$id] } else { $instanceDefs[$id][1] }
        $fields[$id] = Add-Field $grid $instanceDefs[$id][0] $value
    }
    $admin = Add-Field $grid 'New administrator' $Options.AdminUser
    $secret = Add-Field $grid 'New administrator password' ''
    $secret.UseSystemPasswordChar = $true
    $switches = New-Object Windows.Forms.FlowLayoutPanel
    $switches.Width = 600
    $switches.Height = 42
    $remote = Add-Check $switches 'Allow remote bind' ([bool]$Options.Instance.allowRemote)
    $service = Add-Check $switches 'Install Windows service' ([bool]$Options.Instance.installService)
    $firewall = Add-Check $switches 'Create scoped firewall rule' ([bool]$Options.Instance.firewall)
    $grid.RowCount++
    $grid.Controls.Add($switches,1,$grid.RowCount-1)

    $applyPage = Add-Page 'Review and apply'
    $review = Add-Text $applyPage 'Check the plan before applying.' $true
    $controls = New-Object Windows.Forms.FlowLayoutPanel
    $controls.Dock = 'Bottom'
    $controls.Height = 55
    $preview = New-Object Windows.Forms.Button
    $preview.Text = 'Check plan'
    $preview.AutoSize = $true
    $applyBtn = New-Object Windows.Forms.Button
    $applyBtn.Text = 'Apply reviewed plan'
    $applyBtn.AutoSize = $true
    $applyBtn.Enabled = $false
    $cancel = New-Object Windows.Forms.Button
    $cancel.Text = 'Cancel'
    $bar = New-Object Windows.Forms.ProgressBar
    $bar.Width = 260
    $controls.Controls.AddRange(@($preview,$applyBtn,$cancel,$bar))
    $applyPage.Controls.Add($controls)
    $stateInfo = @{ running=$false; cancelled=$false; reviewed='' }

    $readOptions = {
        $chosen = $Options.Clone()
        $chosen.Operation = [string]$operation.SelectedItem
        $chosen.Profile = $null
        $chosen.ComponentId = @()
        if ($preset.SelectedItem -eq 'Custom') {
            $chosen.ComponentId = @($components.CheckedIndices | ForEach-Object { $ids[$_] })
        } else { $chosen.Profile = $presets[[string]$preset.SelectedItem] }
        $chosen.AllowDevelopment = $development.Checked
        $chosen.InstallRoot = $program.Text.Trim()
        $chosen.StateDirectory = $state.Text.Trim()
        $chosen.PortableDirectory = $portable.Text.Trim()
        $chosen.AdminUser = $admin.Text.Trim()
        $chosen.Instance = $null
        if ($fields.instanceRoot.Text.Trim()) {
            $chosen.Instance = @{ allowRemote=$remote.Checked; installService=$service.Checked; firewall=$firewall.Checked }
            foreach ($id in $fields.Keys) { if ($fields[$id].Text.Trim()) { $chosen.Instance[$id] = $fields[$id].Text.Trim() } }
            $chosen.Instance.port = [int]$chosen.Instance.port
        }
        $chosen
    }
    $fingerprint = {
        param($Chosen)
        $visible = $Chosen.Clone()
        foreach ($field in @('AdminPassword','OnProgress','IsCancelled')) { $visible.Remove($field) }
        $visible | ConvertTo-Json -Depth 6 -Compress
    }
    $preview.Add_Click({
        try {
            $chosen = & $readOptions
            if ($chosen.Operation -eq 'Portable') {
                if (!$chosen.PortableDirectory) { throw 'Choose a portable Workbench directory.' }
                $review.Text = 'Portable destination ' + $chosen.PortableDirectory
            } else {
                $plan = & $ResolvePlan $chosen
                $preflight = Get-SdpSetupPreflight -Plan $plan -InstallRoot $chosen.InstallRoot
                if (!$preflight.ok) { throw ($preflight.issues -join [Environment]::NewLine) }
                $instanceSummary = if ($chosen.Instance) {
                    $chosen.Instance | Select-Object -Property instanceRoot,instanceName,bind,port,installService,firewall
                }
                $review.Text = [ordered]@{
                    operation=$chosen.Operation; programRoot=$chosen.InstallRoot
                    packages=@($plan.products | ForEach-Object { [ordered]@{ file=$_.artifact; scope=$_.scope; features=$_.features } })
                    diskBytes=$plan.installedBytes; availableDiskBytes=$preflight.availableDiskBytes
                    administrator=$preflight.elevated; pendingReboot=$preflight.pendingReboot; restart=$plan.restart
                    instance=$instanceSummary; systemPath='unchanged'
                } | ConvertTo-Json -Depth 6
            }
            $stateInfo.reviewed = & $fingerprint $chosen
            $applyBtn.Enabled = $true
        } catch { $applyBtn.Enabled = $false; $review.Text = $_.Exception.Message }
    })
    $cancel.Add_Click({
        if ($stateInfo.running) { $stateInfo.cancelled = $true } else { $form.Close() }
    })
    $form.Add_FormClosing({
        param($sender,$event)
        if ($stateInfo.running) { $stateInfo.cancelled = $true; $event.Cancel = $true }
    })
    $applyBtn.Add_Click({
        $passwordValue = $null
        try {
            $chosen = & $readOptions
            if ((& $fingerprint $chosen) -cne $stateInfo.reviewed) { throw 'Check the plan again after changing the selection.' }
            if ($secret.Text) {
                $passwordValue = New-Object Security.SecureString
                foreach ($character in $secret.Text.ToCharArray()) { $passwordValue.AppendChar($character) }
                $passwordValue.MakeReadOnly()
                $chosen.AdminPassword = $passwordValue
                $secret.Clear()
            }
            $stateInfo.running = $true
            $stateInfo.cancelled = $false
            $applyBtn.Enabled = $false
            $preview.Enabled = $false
            $bar.Value = 0
            $chosen.IsCancelled = { [Windows.Forms.Application]::DoEvents(); $stateInfo.cancelled }.GetNewClosure()
            $chosen.OnProgress = {
                param($progress)
                $review.AppendText([Environment]::NewLine + $progress.phase)
                if ($progress.total -gt 0) { $bar.Value = [Math]::Min(100,[int](100*$progress.completed/$progress.total)) }
                [Windows.Forms.Application]::DoEvents()
            }.GetNewClosure()
            $result = & $Apply $chosen
            $bar.Value = 100
            $review.Text = $result | ConvertTo-Json -Depth 8
        } catch { $review.Text = $_.Exception.Message }
        finally {
            $stateInfo.running = $false
            $preview.Enabled = $true
            if ($passwordValue) { $passwordValue.Dispose() }
        }
    })
    if ($Smoke) {
        $form.ShowInTaskbar = $false
        $form.Opacity = 0
        $timer = New-Object Windows.Forms.Timer
        $timer.Interval = 100
        $probe = @{ passed = $false; detail = '' }
        $timer.Add_Tick({
            $timer.Stop()
            $preset.SelectedItem = 'Workbench Only'
            $development.Checked = $true
            $tabControl.SelectedTab = $applyPage
            $preview.PerformClick()
            $probe.passed = $applyBtn.Enabled -and $review.Text -match 'workbench'
            $probe.detail = $review.Text
            $form.Close()
        })
        $form.Add_Shown({ $timer.Start() })
        if ($tabControl.TabPages.Count -ne 4 -or !$components.Items.Count) { throw 'Setup window smoke failed.' }
    }
    try { [void]$form.ShowDialog() }
    finally {
        if ($Smoke) { $timer.Dispose() }
        $form.Dispose()
    }
    if ($Smoke -and !$probe.passed) { throw ('Setup review smoke failed. ' + $probe.detail) }
}

Export-ModuleMember -Function Show-SdpSetupWindow
