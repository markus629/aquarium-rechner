#ifndef WEBPAGE_H
#define WEBPAGE_H

const char WEBPAGE_HTML[] PROGMEM = R"=====(
<!DOCTYPE html>
<html lang="de">
   <head>
      <meta charset="UTF-8">
      <meta name="viewport" content="width=device-width, initial-scale=1.0">
      <title>Dosierpumpen-Steuerung</title>
<!-- Import Vue.js (OFFLINE - von ESP32 bereitgestellt) -->
<script src="/vue.js"></script>


<style>
         :root {
         --primary-color: #2196F3;
         --primary-dark: #0d75d1;
         --secondary-color: #f0f0f0;
         --text-color: #333;
         --border-color: #ddd;
         --success-color: #4CAF50;
         --warning-color: #ff9800;
         --danger-color: #f44336;
         --info-color: #03a9f4;
         --card-shadow: 0 2px 10px rgba(0, 0, 0, 0.08);
         --transition-speed: 0.3s;
         }

         /* Speicher-Anzeige im Header */
         .memory-status {
             display: grid;
             grid-template-columns: 1fr 1fr;
             gap: 1px 8px;
             font-size: 9px;
             font-family: 'Courier New', monospace;
             padding: 4px 6px;
             background: rgba(0, 0, 0, 0.05);
             border-radius: 4px;
             min-width: 130px;
             border: 1px solid rgba(0, 0, 0, 0.1);
         }

         .memory-item {
             display: flex;
             justify-content: space-between;
             align-items: center;
             line-height: 1.2;
         }

         .memory-label {
             font-weight: bold;
             margin-right: 4px;
             font-size: 8px;
         }

         .memory-value {
             font-weight: normal;
             font-size: 9px;
         }

         /* Farbkodierung für Speicher-Status */
         .status-good {
             color: #4CAF50;
             border-color: #4CAF50;
             background: rgba(76, 175, 80, 0.1);
         }

         .status-warning {
             color: #FF9800;
             border-color: #FF9800;
             background: rgba(255, 152, 0, 0.1);
         }

         .status-critical {
             color: #F44336;
             border-color: #F44336;
             background: rgba(244, 67, 54, 0.1);
             animation: blink 1.5s infinite;
         }

         @keyframes blink {
             0%, 50% { opacity: 1; }
             51%, 100% { opacity: 0.6; }
         }

         /* Responsive: Auf kleinen Bildschirmen verstecken */
         @media (max-width: 768px) {
             .memory-status {
                 display: none;
             }
         }

         * {
         box-sizing: border-box;
         margin: 0;
         padding: 0;
         }
         body {
         font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
         line-height: 1.6;
         color: var(--text-color);
         background-color: #f5f5f5;
         margin: 0;
         padding: 0;
         }
         .container {
         max-width: 1200px;
         margin: 0 auto;
         padding: 20px;
         }
         .dashboard-header {
         background-color: white;
         border-radius: 10px;
         padding: 20px;
         margin-bottom: 20px;
         box-shadow: var(--card-shadow);
         display: flex;
         justify-content: space-between;
         align-items: center;
         flex-wrap: wrap;
         }
         .header-status-container {
         display: flex;
         align-items: center;
         justify-content: space-between;
         gap: 15px;
         margin-top: 10px;
         width: 100%;
         }
         .clock-container {
         font-size: 1rem;
         font-weight: 600;
         color: #555;
         display: flex;
         align-items: center;
         gap: 8px;
         }
         .ntp-status {
         font-size: 1.1rem;
         cursor: help;
         }
         .ntp-status.synced {
         color: var(--success-color);
         }
         .ntp-status.unsynced {
         color: var(--danger-color);
         }
         .dashboard-title {
         color: var(--primary-color);
         margin: 0;
         font-size: 1.8rem;
         }
         .status-indicator {
         padding: 8px 15px;
         border-radius: 20px;
         font-weight: 600;
         display: inline-flex;
         align-items: center;
         margin-top: 10px;
         }
         .status-indicator.connected {
         background-color: rgba(76, 175, 80, 0.2);
         color: var(--success-color);
         }
         .status-indicator.disconnected {
         background-color: rgba(244, 67, 54, 0.2);
         color: var(--danger-color);
         }
         .status-indicator::before {
         content: "";
         display: inline-block;
         width: 10px;
         height: 10px;
         border-radius: 50%;
         margin-right: 8px;
         }
         .status-indicator.connected::before {
         background-color: var(--success-color);
         }
         .status-indicator.disconnected::before {
         background-color: var(--danger-color);
         }
         .message-box {
         padding: 15px;
         margin: 15px 0;
         border-radius: 8px;
         }
         .message-box.success {
         background-color: rgba(76, 175, 80, 0.2);
         color: var(--success-color);
         border-left: 4px solid var(--success-color);
         }
         .message-box.error {
         background-color: rgba(244, 67, 54, 0.2);
         color: var(--danger-color);
         border-left: 4px solid var(--danger-color);
         }
         .message-box.info {
         background-color: rgba(3, 169, 244, 0.2);
         color: var(--info-color);
         border-left: 4px solid var(--info-color);
         }
         /* Tabs Styling */
         .tabs {
         display: flex;
         border-bottom: 1px solid var(--border-color);
         margin-bottom: 20px;
         flex-wrap: wrap;
         }
         .tab-link {
         padding: 12px 24px;
         cursor: pointer;
         font-weight: 600;
         position: relative;
         transition: color var(--transition-speed);
         color: #777;
         }
         .tab-link:hover {
         color: var(--primary-color);
         }
         .tab-link.active {
         color: var(--primary-color);
         }
         .tab-link.active::after {
         content: '';
         position: absolute;
         bottom: -1px;
         left: 0;
         right: 0;
         height: 3px;
         background-color: var(--primary-color);
         border-radius: 3px 3px 0 0;
         }
         .tab-content {
         display: none;
         }
         .tab-content.active {
         display: block;
         animation: fadeIn 0.3s ease;
         }
         @keyframes fadeIn {
         from { opacity: 0; }
         to { opacity: 1; }
         }
         /* Card Styling */
         .card {
         background-color: white;
         border-radius: 10px;
         padding: 20px;
         margin-bottom: 20px;
         box-shadow: var(--card-shadow);
         }
         .card-header {
         display: flex;
         justify-content: space-between;
         align-items: center;
         margin-bottom: 15px;
         padding-bottom: 15px;
         border-bottom: 1px solid var(--border-color);
         }
         .card-title {
         font-size: 1.2rem;
         color: var(--primary-color);
         margin: 0;
         }
         /* Form Elements */
         .form-group {
         margin-bottom: 15px;
         }
         .form-row {
         display: flex;
         flex-wrap: wrap;
         gap: 15px;
         margin-bottom: 15px;
         }
         .form-column {
         flex: 1;
         min-width: 250px;
         }
         label {
         display: block;
         margin-bottom: 8px;
         font-weight: 600;
         color: #555;
         }
         input, select {
         width: 100%;
         padding: 10px 12px;
         border: 1px solid var(--border-color);
         border-radius: 6px;
         font-size: 0.95rem;
         transition: border-color var(--transition-speed);
         }
         input:focus, select:focus {
         outline: none;
         border-color: var(--primary-color);
         box-shadow: 0 0 0 2px rgba(33, 150, 243, 0.2);
         }
         .help-text {
         display: block;
         font-size: 0.85rem;
         color: #777;
         margin-top: 5px;
         }
         button {
         background-color: var(--primary-color);
         color: white;
         border: none;
         border-radius: 6px;
         padding: 10px 16px;
         font-weight: 600;
         cursor: pointer;
         transition: background-color var(--transition-speed);
         }
         button:hover {
         background-color: var(--primary-dark);
         }
         button.danger {
         background-color: var(--danger-color);
         }
         button.danger:hover {
         background-color: #d32f2f;
         }
         /* Table Styling */
         .table-container {
         overflow-x: auto;
         margin-bottom: 20px;
         }
         table {
         width: 100%;
         border-collapse: collapse;
         margin-bottom: 0;
         }
         th, td {
         padding: 12px 15px;
         text-align: left;
         border-bottom: 1px solid var(--border-color);
         }
         th {
         background-color: rgba(33, 150, 243, 0.05);
         color: #555;
         font-weight: 600;
         }
         tr:hover {
         background-color: rgba(0, 0, 0, 0.01);
         }
         /* Collapsible Sections */
         .collapsible-section {
         margin-bottom: 15px;
         border: 1px solid var(--border-color);
         border-radius: 8px;
         overflow: hidden;
         }
         .collapsible-header {
         padding: 15px;
         cursor: pointer;
         background-color: rgba(33, 150, 243, 0.05);
         transition: background-color var(--transition-speed);
         display: flex;
         justify-content: space-between;
         align-items: center;
         }
         .collapsible-header:hover {
         background-color: rgba(33, 150, 243, 0.1);
         }
         .collapsible-header h3 {
         margin: 0;
         font-size: 1.1rem;
         color: #444;
         }
         .collapsible-arrow {
         transition: transform var(--transition-speed);
         font-size: 1.2rem;
         }
         .collapsible-content {
         padding: 15px;
         border-top: 1px solid var(--border-color);
         }
         .collapsible-content.collapsed {
         display: none;
         }
         /* Action Button Styling */
         .action-button {
         display: inline-flex;
         align-items: center;
         justify-content: center;
         width: 32px;
         height: 32px;
         border-radius: 4px;
         transition: background-color var(--transition-speed);
         }
         .delete-btn {
         color: var(--danger-color);
         cursor: pointer;
         }
         .delete-btn:hover {
         background-color: rgba(244, 67, 54, 0.1);
         }
         .adopt-btn {
         cursor: pointer;
         font-size: 18px;
         font-weight: bold;
         }
         .adopt-btn:hover {
         background-color: rgba(76, 175, 80, 0.1);
         }
         /* Container Monitoring */
         .progress-container {
         width: 100%;
         background-color: #f1f1f1;
         border-radius: 6px;
         position: relative;
         height: 24px;
         overflow: hidden;
         }
         .progress-bar {
         height: 100%;
         border-radius: 6px;
         transition: width 0.5s ease;
         }
         .progress-text {
         position: absolute;
         top: 50%;
         left: 50%;
         transform: translate(-50%, -50%);
         color: black;
         font-weight: bold;
         font-size: 0.8rem;
         text-shadow: 1px 1px 0 rgba(255,255,255,0.7);
         }
         /* Toggle Switch */
         .switch {
         position: relative;
         display: inline-block;
         width: 70px;
         height: 34px;
         min-width: 70px;
         }
         .switch input {
         opacity: 0;
         width: 0;
         height: 0;
         }
         .slider {
         position: absolute;
         cursor: pointer;
         top: 0;
         left: 0;
         right: 0;
         bottom: 0;
         background-color: #ccc;
         transition: .4s;
         border-radius: 34px;
         box-sizing: border-box;
         }
         .slider:before {
         position: absolute;
         content: "";
         height: 26px;
         width: 26px;
         left: 4px;
         bottom: 4px;
         background-color: white;
         transition: .4s;
         border-radius: 50%;
         box-shadow: 0 2px 4px rgba(0,0,0,0.2);
         }
         input:checked + .slider {
         background-color: var(--primary-color);
         }
         input:checked + .slider:before {
         transform: translateX(36px);
         }
         /* Value Highlights */
         .highlight-good {
         color: var(--success-color);
         font-weight: bold;
         }
         .highlight-warning {
         color: var(--warning-color);
         font-weight: bold;
         }
         .highlight-bad {
         color: var(--danger-color);
         font-weight: bold;
         }
         /* Dashboard Widgets */
         .dashboard-widgets {
         display: grid;
         grid-template-columns: repeat(auto-fit, minmax(300px, 1fr));
         gap: 20px;
         margin-bottom: 20px;
         }
         .widget {
         background-color: white;
         border-radius: 10px;
         padding: 20px;
         box-shadow: var(--card-shadow);
         }
         .widget-title {
         font-size: 1rem;
         color: #555;
         margin-bottom: 15px;
         }
         .widget-value {
         font-size: 2rem;
         font-weight: 700;
         margin-bottom: 5px;
         }
         .widget-label {
         font-size: 0.9rem;
         color: #777;
         }
         .value-pair {
         display: flex;
         justify-content: space-between;
         margin-bottom: 10px;
         }
         .value-label {
         font-weight: 600;
         color: #555;
         }
         .value-data {
         font-weight: 600;
         }

         /* Navigationstasten für Dosierplan */
         .day-navigation {
         display: flex;
         align-items: center;
         gap: 15px;
         }
         .nav-btn {
         background-color: var(--primary-color);
         color: white;
         border: none;
         border-radius: 50%;
         width: 30px;
         height: 30px;
         display: flex;
         align-items: center;
         justify-content: center;
         cursor: pointer;
         font-weight: bold;
         }
         .nav-btn:disabled {
         background-color: #cccccc;
         cursor: not-allowed;
         }
         button:disabled {
         opacity: 0.4;
         cursor: not-allowed;
         pointer-events: none;
         }
         
         /* Responsive adjustments */
         @media (max-width: 768px) {
         .dashboard-header {
         flex-direction: column;
         align-items: flex-start;
         }
         .header-status-container {
         margin-top: 10px;
         width: 100%;
         flex-direction: row;
         justify-content: space-between;
         align-items: center;
         }
         .tabs {
         overflow-x: auto;
         white-space: nowrap;
         padding-bottom: 5px;
         }
         .tab-link {
         padding: 10px 15px;
         }
         .form-column {
         min-width: 100%;
         }
         /* Styles für den Fortschrittsbalken */
         .progress-container {
         margin: 15px 0;
         padding: 10px;
         background-color: #f0f8ff;
         border: 1px solid #b8daff;
         border-radius: 5px;
         }
         .progress-label {
         font-weight: bold;
         margin-bottom: 5px;
         }
         .progress-container .progress-container {
         width: 100%;
         height: 20px;
         background-color: #e9ecef;
         border-radius: 4px;
         overflow: hidden;
         margin-bottom: 5px;
         }
         .progress-bar {
         height: 100%;
         background-color: #007bff;
         width: 0%;
         transition: width 0.3s ease;
         }
         .progress-info {
         display: flex;
         justify-content: space-between;
         font-size: 0.9em;
         color: #666;
         }
         }
         
         /* Styles für den pH-Kalibrierungsfortschritt */
         .calibration-progress {
         margin: 15px 0;
         padding: 10px;
         background-color: #f0f8ff;
         border: 1px solid #b8daff;
         border-radius: 5px;
         }
         .voltage-display {
         margin-top: 10px;
         padding: 8px;
         background-color: #f8f9fa;
         border-radius: 4px;
         border: 1px solid #ddd;
         font-weight: bold;
         }
         .calibration-info {
         margin-top: 5px;
         font-size: 0.8em;
         color: #666;
         line-height: 1.3;
         }
         
         /* Berechnungserklärung Stile */
         .explanation-block {
         background-color: #f8f9fa;
         border-radius: 8px;
         padding: 15px;
         margin-bottom: 15px;
         }
         .explanation-title {
         font-weight: 600;
         margin-bottom: 8px;
         color: var(--primary-color);
         }
         .explanation-formula {
         font-family: monospace;
         background-color: #e9ecef;
         padding: 8px;
         border-radius: 4px;
         margin: 8px 0;
         }
      </style>
   </head>
<body>


<div id="app" class="container">
         <div class="dashboard-header">
            <h1 class="dashboard-title">Dosierpumpen-Steuerung</h1>
            <div class="header-status-container">
               <!-- Speicher-Status ZUERST (ganz links) -->
               <div class="memory-status" v-if="memoryStatus" :class="'status-' + memoryStatus.status">
                  <div class="memory-item">
                     <span class="memory-label">LittleFS:</span>
                     <span class="memory-value">{{Math.round(memoryStatus.littlefs.percentFree)}}%</span>
                  </div>
                  <div class="memory-item">
                     <span class="memory-label">Heap:</span>
                     <span class="memory-value">{{Math.round(memoryStatus.heap.percentFree)}}%</span>
                  </div>
                  <div class="memory-item">
                     <span class="memory-label">Flash:</span>
                     <span class="memory-value">{{Math.round(memoryStatus.flash.percentFree)}}%</span>
                  </div>
                  <div class="memory-item" v-if="memoryStatus.psram">
                     <span class="memory-label">PSRAM:</span>
                     <span class="memory-value">{{Math.round(memoryStatus.psram.percentFree)}}%</span>
                  </div>
               </div>
               
               <!-- Zeit ZENTRAL -->
               <div class="clock-container">
                  <span>{{ currentTime }}</span>
                  <span class="ntp-status" :class="{ synced: timeInitialized, unsynced: !timeInitialized }"
                     :title="getTimeStatusTitle()">
                  {{ timeInitialized ? '✓' : '⟳' }}
                  </span>
               </div>
               
               <!-- Verbindung RECHTS -->
               <div class="status-indicator" :class="{ connected: connected, disconnected: !connected }">
                  {{ connected ? 'Verbunden' : 'Getrennt' }}
               </div>
            </div>
         </div>
         <!-- Tabs Navigation -->
         <div class="tabs">
            <div v-for="tab in tabs" :key="tab.id" class="tab-link" 
               :class="{ active: activeTab === tab.id }" 
               @click="setActiveTab(tab.id)">
               {{ tab.name }}
            </div>
         </div>
         <!-- Dashboard Tab -->
         <div id="dashboardTab" class="tab-content" :class="{ active: activeTab === 'dashboard' }">
            <div class="dashboard-widgets">
               <div class="widget">
                  <h3 class="widget-title">KH-Wert</h3>
                  <div class="widget-value" :class="getValueHighlightClass(systemSettings.currentKH, systemSettings.targetKH, 0.5, 1.0)">
                     {{ systemSettings.currentKH ? systemSettings.currentKH.toFixed(1) : '-' }} °dKH
                  </div>
                  <div class="widget-label">
                     Zielwert: {{ systemSettings.targetKH ? systemSettings.targetKH.toFixed(1) : '-' }} °dKH
                  </div>
               </div>
               <div class="widget">
                  <h3 class="widget-title">Calcium-Wert</h3>
                  <div class="widget-value" :class="getValueHighlightClass(systemSettings.currentCalcium, systemSettings.targetCalcium, 20, 40)">
                     {{ systemSettings.currentCalcium ? systemSettings.currentCalcium.toFixed(0) : '-' }} mg/l
                  </div>
                  <div class="widget-label">
                     Zielwert: {{ systemSettings.targetCalcium ? systemSettings.targetCalcium.toFixed(0) : '-' }} mg/l
                  </div>
               </div>
               <div class="widget">
                  <h3 class="widget-title">Automatische Dosierung</h3>
                  <div class="widget-value" :class="{'highlight-good': systemSettings.autoDosing}">
                     {{ systemSettings.autoDosing ? 'Aktiviert' : 'Deaktiviert' }}
                  </div>
                  <div class="widget-label">
                     Letzte Dosierung: {{ systemSettings.lastAutoDosageFormatted || 'Keine Dosierung' }}
                  </div>
               </div>
            </div>
            <div class="card">
               <div class="card-header">
                  <h2 class="card-title">Nächste Dosierungen</h2>
               </div>
               <div class="table-container">
                  <table>
                     <thead>
                        <tr>
                           <th>Parameter</th>
                           <th>Aktueller Wert</th>
                           <th>Geplante Dosierung</th>
                           <th>Prognose</th>
                        </tr>
                     </thead>
                     <tbody>
                        <tr>
                           <td>KH</td>
                           <td :class="getValueHighlightClass(systemSettings.currentKH, systemSettings.targetKH, 0.5, 1.0)">
                              {{ systemSettings.currentKH ? systemSettings.currentKH.toFixed(1) : '-' }} °dKH
                           </td>
                           <td>{{ systemSettings.nextKHDosage ? systemSettings.nextKHDosage.toFixed(2) : '-' }} ml</td>
<td :class="systemSettings.nextKHProjected ? getValueHighlightClass(systemSettings.nextKHProjected, systemSettings.targetKH, 0.5, 1.0) : ''">
  {{ systemSettings.nextKHProjected ? systemSettings.nextKHProjected.toFixed(1) : '-' }} °dKH
</td>
                        </tr>
                        <tr>
                           <td>Calcium</td>
                           <td :class="getValueHighlightClass(systemSettings.currentCalcium, systemSettings.targetCalcium, 20, 40)">
                              {{ systemSettings.currentCalcium ? systemSettings.currentCalcium.toFixed(0) : '-' }} mg/l
                           </td>
                           <td>{{ systemSettings.nextCalciumDosage ? systemSettings.nextCalciumDosage.toFixed(2) : '-' }} ml</td>
<td :class="systemSettings.nextCalciumProjected ? getValueHighlightClass(systemSettings.nextCalciumProjected, systemSettings.targetCalcium, 20, 40) : ''">
  {{ systemSettings.nextCalciumProjected ? systemSettings.nextCalciumProjected.toFixed(0) : '-' }} mg/l
</td>
                        </tr>
                     </tbody>
                  </table>
               </div>
 </div>
            <div class="card">
<div class="card-header">
                  <h2 class="card-title">{{ getChartTitle() }}</h2>
               </div>
               <div id="consumptionChart" style="width: 100%; height: 300px; position: relative;">
                  <svg width="100%" height="100%" viewBox="0 0 800 300" style="border: 1px solid #ddd; border-radius: 6px;">
                     <!-- Grid lines -->
                     <defs>
                        <pattern id="grid" width="40" height="25" patternUnits="userSpaceOnUse">
                           <path d="M 40 0 L 0 0 0 25" fill="none" stroke="#f0f0f0" stroke-width="1"/>
                        </pattern>
                     </defs>
                     <rect width="100%" height="100%" fill="url(#grid)" />
                     
                     <!-- Chart area -->
                     <g id="chartContent" transform="translate(60, 20)">
                        <!-- Data will be inserted here -->
                     </g>
                     
<!-- Y-Axis Labels -->
<g id="yAxisKH" transform="translate(15, 20)">
  <text x="0" y="10" font-size="12" fill="#2196F3" text-anchor="middle" font-weight="bold">KH</text>
  <text x="0" y="25" font-size="10" fill="#2196F3" text-anchor="middle">(dKH/Tag)</text>
</g>
<g id="yAxisCa" transform="translate(785, 20)">
  <text x="0" y="10" font-size="12" fill="#ff9800" text-anchor="middle" font-weight="bold">Ca</text>
  <text x="0" y="25" font-size="10" fill="#ff9800" text-anchor="middle">(mg/l/Tag)</text>
</g>
                     
                     <!-- X-Axis -->
                     <g id="xAxis" transform="translate(60, 280)">
                        <!-- Date labels will be inserted here -->
                     </g>
                  </svg>
                  <div v-if="!consumptionData.length" style="position: absolute; top: 50%; left: 50%; transform: translate(-50%, -50%); color: #999;">
                     Lade Verbrauchsdaten...
</div>
               </div>
            </div>
            <div class="card">
               <div class="card-header">
                  <h2 class="card-title">pH-Trend-Analyse</h2>
               </div>
               <div id="phTrendChart" style="width: 100%; height: 300px; position: relative;">
                  <svg width="100%" height="100%" viewBox="0 0 800 300" style="border: 1px solid #ddd; border-radius: 6px;">
                     <!-- Grid lines -->
                     <defs>
                        <pattern id="phGrid" width="40" height="25" patternUnits="userSpaceOnUse">
                           <path d="M 40 0 L 0 0 0 25" fill="none" stroke="#f0f0f0" stroke-width="1"/>
                        </pattern>
                     </defs>
                     <rect width="100%" height="100%" fill="url(#phGrid)" />
                     
                     <!-- Chart area -->
                     <g id="phChartContent" transform="translate(60, 20)">
                        <!-- Data will be inserted here -->
                     </g>
                     
                     <!-- Y-Axis Labels -->
                     <g id="phYAxis" transform="translate(15, 20)">
                        <text x="0" y="10" font-size="12" fill="#4CAF50" text-anchor="middle" font-weight="bold">pH</text>
                        <text x="0" y="25" font-size="10" fill="#4CAF50" text-anchor="middle">Wert</text>
                     </g>
                     
                     <!-- X-Axis -->
                     <g id="phXAxis" transform="translate(60, 280)">
                        <!-- Date labels will be inserted here -->
                     </g>
                  </svg>
                  <div v-if="!phTrendData.length" style="position: absolute; top: 50%; left: 50%; transform: translate(-50%, -50%); color: #999;">
                     Lade pH-Trend-Daten...
                  </div>
               </div>
            </div>
            <div class="card">
               <div class="card-header">
                  <h2 class="card-title">Behälterstatus</h2>
               </div>
               <div>
                  <div v-for="container in systemSettings.containers" :key="container.id" 
                     style="margin-bottom: 15px;">
                     <div style="display: flex; justify-content: space-between; margin-bottom: 5px;">
                        <div style="font-weight: 600;">{{ container.name }}</div>
                        <div v-html="formatDaysRemaining(container.daysRemaining)"></div>
                     </div>
                     <div class="progress-container">
                        <div class="progress-bar" 
                           :style="{ width: container.percentage + '%', 
                           backgroundColor: getContainerColor(container.percentage) }">
                        </div>
                        <div class="progress-text">
                           {{ Math.round(container.level) }}ml / {{ Math.round(container.capacity) }}ml
                        </div>
                     </div>
                  </div>
               </div>
            </div>
         </div>
         <!-- Wasserparameter Tab -->
         <div id="kalkhaushalTab" class="tab-content" :class="{ active: activeTab === 'kalkhaushalt' }">
            <div class="card">
               <div class="card-header">
                  <h2 class="card-title">Neue Messwerte eintragen</h2>
               </div>
               <div class="form-row">
                  <div class="form-column">
                     <div class="form-group">
                        <label for="khValue">KH-Wert (°dKH):</label>
                        <input type="number" id="khValue" v-model="newMeasurement.kh" step="0.1" min="0" placeholder="KH-Wert eingeben">
                     </div>
                  </div>
                  <div class="form-column">
                     <div class="form-group">
                        <label for="calciumValue">Calcium-Wert (mg/l):</label>
                        <input type="number" id="calciumValue" v-model="newMeasurement.calcium" step="1" min="0" placeholder="Calcium-Wert eingeben">
                     </div>
                  </div>
               </div>
<div class="form-group">
  <button @click="saveWaterMeasurement">Messwerte speichern</button>
</div>
            </div>
            <div class="card">
               <div class="card-header">
                  <h2 class="card-title">Messwerte Verlauf</h2>
               </div>
               <!-- KH Tabelle -->
               <div class="collapsible-section">
                  <div class="collapsible-header" @click="toggleCollapsible('khMeasurements')">
                     <h3>KH Messungen</h3>
                     <span class="collapsible-arrow" :style="{ transform: collapsible.khMeasurements ? 'rotate(0deg)' : 'rotate(-90deg)' }">▼</span>
                  </div>
                  <div class="collapsible-content" :class="{ collapsed: !collapsible.khMeasurements }">
                     <div class="table-container">
                        <table>
                           <thead>
                              <tr>
                                 <th>Datum</th>
                                 <th>KH (°dKH)</th>
                                 <th>Aktion</th>
                              </tr>
                           </thead>
                           <tbody>
                              <tr v-if="!waterMeasurements.kh.length">
                                 <td colspan="3" style="text-align: center">Keine KH-Daten vorhanden</td>
                              </tr>
                              <tr v-for="measurement in waterMeasurements.kh" :key="measurement.index">
                                 <td>{{ measurement.date }}</td>
                                 <td>{{ measurement.value.toFixed(1) }}</td>
                                 <td>
                                    <span class="delete-btn action-button" @click="deleteWaterMeasurement(measurement.index, false)">✕</span>
                                 </td>
                              </tr>
                           </tbody>
                        </table>
                     </div>
                  </div>
               </div>
               <!-- Calcium Tabelle -->
               <div class="collapsible-section">
                  <div class="collapsible-header" @click="toggleCollapsible('caMeasurements')">
                     <h3>Calcium Messungen</h3>
                     <span class="collapsible-arrow" :style="{ transform: collapsible.caMeasurements ? 'rotate(0deg)' : 'rotate(-90deg)' }">▼</span>
                  </div>
                  <div class="collapsible-content" :class="{ collapsed: !collapsible.caMeasurements }">
                     <div class="table-container">
                        <table>
                           <thead>
                              <tr>
                                 <th>Datum</th>
                                 <th>Calcium (mg/l)</th>
                                 <th>Aktion</th>
                              </tr>
                           </thead>
                           <tbody>
                              <tr v-if="!waterMeasurements.calcium.length">
                                 <td colspan="3" style="text-align: center">Keine Calcium-Daten vorhanden</td>
                              </tr>
                              <tr v-for="measurement in waterMeasurements.calcium" :key="measurement.index">
                                 <td>{{ measurement.date }}</td>
                                 <td>{{ measurement.value.toFixed(0) }}</td>
                                 <td>
                                    <span class="delete-btn action-button" @click="deleteWaterMeasurement(measurement.index, true)">✕</span>
                                 </td>
                              </tr>
                           </tbody>
                        </table>
                     </div>
                  </div>
               </div>
               <!-- Schwankungskompensation -->
               <div class="card" style="margin-bottom: 15px; padding: 15px;">
                  <div style="display: flex; align-items: center; gap: 15px; flex-wrap: wrap;">
                     <h3 style="margin: 0;">Schwankungskompensation</h3>
                     <label class="switch">
                        <input type="checkbox" v-model="dosingFactors.enabled" @change="toggleDosingFactors">
                        <span class="slider"></span>
                     </label>
                     <span>{{ dosingFactors.enabled ? 'Aktiv' : 'Inaktiv' }}</span>
                     <button @click="calculateDosingFactors" :disabled="!dosingFactors.hasNewPattern"
                        class="btn" style="padding: 5px 12px; font-size: 13px;">
                        Neuberechnen
                     </button>
                     <button @click="resetDosingFactors"
                        class="btn" style="padding: 5px 12px; font-size: 13px; background: #f44336;">
                        Reset
                     </button>
                     <small style="color: #888; font-style: italic;">Neuberechnen aktiv bei mind. 6 KH-Messungen, max. 4h Abstand, mind. 20h Zeitspanne</small>
                  </div>
                  <div v-if="dosingFactors.factors.some(f => f !== 1.0)" style="margin-top: 10px;">
                     <small>Aktuelle Faktoren pro 2h-Intervall:</small>
                     <div style="display: flex; gap: 4px; flex-wrap: wrap; margin-top: 5px;">
                        <span v-for="(f, i) in dosingFactors.factors" :key="i"
                           style="padding: 2px 6px; border-radius: 4px; font-size: 12px;"
                           :style="{ background: f > 1.01 ? '#4CAF50' : f < 0.99 ? '#ff9800' : '#e0e0e0', color: (f > 1.01 || f < 0.99) ? 'white' : '#333' }">
                           {{ (i*2) }}-{{ (i*2+2) }}h: {{ f.toFixed(2) }}
                        </span>
                     </div>
                  </div>
               </div>

               <!-- NEU: Automatische KH Tabelle -->
               <div class="collapsible-section">
                  <div class="collapsible-header" @click="toggleCollapsible('autoKhMeasurements')">
                     <h3>Automatische KH Messungen (Referenz)</h3>
                     <span class="collapsible-arrow" :style="{ transform: collapsible.autoKhMeasurements ? 'rotate(0deg)' : 'rotate(-90deg)' }">▼</span>
                  </div>
                  <div class="collapsible-content" :class="{ collapsed: !collapsible.autoKhMeasurements }">
                     <div class="table-container">
                        <table>
                           <thead>
                              <tr>
                                 <th>Datum</th>
                                 <th>KH (°dKH)</th>
                                 <th>Aktion</th>
                              </tr>
                           </thead>
                           <tbody>
                              <tr v-if="!waterMeasurements.autoKh.length">
                                 <td colspan="3" style="text-align: center">Keine automatischen KH-Daten vorhanden</td>
                              </tr>
                              <tr v-for="measurement in waterMeasurements.autoKh" :key="measurement.index">
                                 <td>{{ measurement.date }}</td>
                                 <td>{{ measurement.value.toFixed(1) }}</td>
                                 <td>
                                    <span class="adopt-btn action-button"
                                       style="color: #4CAF50"
                                       @click="adoptAutoKHMeasurement(measurement.index, measurement.value)"
                                       title="In KH-Messungen übernehmen">↳</span>
                                    <span class="delete-btn action-button" @click="deleteAutoKHMeasurement(measurement.index)">✕</span>
                                 </td>
                              </tr>
                           </tbody>
                        </table>
                     </div>
                  </div>
               </div>
            </div>
         </div>
         <!-- Dosiersystem Tab -->
         <div id="dosageTab" class="tab-content" :class="{ active: activeTab === 'dosage' }">
            <div class="card">
               <div class="card-header">
                  <h2 class="card-title">Automatische Dosierung</h2>
               </div>
               <div class="form-group" style="display: flex; align-items: center; gap: 15px;">
                  <label for="autoDosing">Automatische Dosierung aktivieren:</label>
                  <label class="switch">
                  <input type="checkbox" id="autoDosing" v-model="systemSettings.autoDosing" @change="toggleAutoDosing">
                  <span class="slider"></span>
                  </label>
               </div>
               <div v-if="systemSettings.lastAutoDosageFormatted" class="message-box info">
                  Letzte automatische Dosierung: {{ systemSettings.lastAutoDosageFormatted }}
               </div>
               <div class="form-group" style="margin-top: 20px;">
                  <h3>Aktuelle Verbrauchswerte</h3>
                  <div class="table-container">
                     <table>
                        <thead>
                           <tr>
                              <th>Parameter</th>
                              <th>Aktueller Wert</th>
                              <th>Zielwert</th>
                              <th>Täglicher Verbrauch</th>
                              <th>Nächste Dosierung</th>
                           </tr>
                        </thead>
                        <tbody>
                           <tr>
                              <td>KH</td>
                              <td :class="getValueHighlightClass(systemSettings.currentKH, systemSettings.targetKH, 0.5, 1.0)">
                                 {{ systemSettings.currentKH ? systemSettings.currentKH.toFixed(1) : '-' }} °dKH
                              </td>
                              <td>{{ systemSettings.targetKH ? systemSettings.targetKH.toFixed(1) : '-' }} °dKH</td>
                              <td>{{ systemSettings.dailyKHConsumption > 0 ? systemSettings.dailyKHConsumption.toFixed(2) + ' ml/Tag' : 'N/A' }}</td>
                              <td>{{ systemSettings.nextKHDosage ? systemSettings.nextKHDosage.toFixed(2) + ' ml' : '-' }}</td>
                           </tr>
                           <tr>
                              <td>Calcium</td>
                              <td :class="getValueHighlightClass(systemSettings.currentCalcium, systemSettings.targetCalcium, 20, 40)">
                                 {{ systemSettings.currentCalcium ? systemSettings.currentCalcium.toFixed(0) : '-' }} mg/l
                              </td>
                              <td>{{ systemSettings.targetCalcium ? systemSettings.targetCalcium.toFixed(0) : '-' }} mg/l</td>
                              <td>{{ systemSettings.dailyCalciumConsumption > 0 ? systemSettings.dailyCalciumConsumption.toFixed(2) + ' ml/Tag' : 'N/A' }}</td>
                              <td>{{ systemSettings.nextCalciumDosage ? systemSettings.nextCalciumDosage.toFixed(2) + ' ml' : '-' }}</td>
                           </tr>
                        </tbody>
                     </table>
                  </div>
               </div>
            </div>
<!-- KH-Dosierplan Tabelle -->
<div class="card">
  <div class="card-header">
    <h2 class="card-title">KH-Dosierplan (Vollständig)</h2>
  </div>
  <div class="table-container">
    <div v-if="!systemSettings.autoDosing" class="message-box info" style="text-align: center; padding: 15px;">
      Automatische Dosierung ist deaktiviert. Keine Dosierungen geplant.
    </div>
    <table v-else class="dosage-plan-table">
      <thead>
        <tr>
          <th>Datum/Zeit</th>
          <th>KH-Dosis (ml)</th>
          <th>Prognose KH</th>
          <th>Typ</th>
        </tr>
      </thead>
      <tbody>
        <tr v-if="!khDosagePlan.length">
          <td colspan="4" style="text-align: center;">Keine KH-Dosierungen geplant</td>
        </tr>
        <tr v-for="(entry, index) in khDosagePlan" :key="'kh-'+index">
          <td>{{ entry.formattedDate }}</td>
          <td style="text-align: right;">{{ entry.dosage !== null && entry.dosage !== undefined ? entry.dosage.toFixed(2) : '-' }}</td>
          <td :class="entry.projectedValue !== null ? getValueHighlightClass(entry.projectedValue, systemSettings.targetKH, 0.5, 1.0) : ''" style="text-align: right;">
            {{ entry.projectedValue !== null ? entry.projectedValue.toFixed(1) : '-' }}
          </td>
          <td>{{ entry.type }}</td>
        </tr>
      </tbody>
    </table>
  </div>
</div>

<!-- Calcium-Dosierplan Tabelle -->
<div class="card">
  <div class="card-header">
    <h2 class="card-title">Calcium-Dosierplan (Vollständig)</h2>
  </div>
  <div class="table-container">
    <div v-if="!systemSettings.autoDosing" class="message-box info" style="text-align: center; padding: 15px;">
      Automatische Dosierung ist deaktiviert. Keine Dosierungen geplant.
    </div>
    <table v-else class="dosage-plan-table">
      <thead>
        <tr>
          <th>Datum/Zeit</th>
          <th>Ca-Dosis (ml)</th>
          <th>Mg-Dosis (ml)</th>
          <th>Prognose Ca</th>
          <th>Typ</th>
        </tr>
      </thead>
      <tbody>
        <tr v-if="!caDosagePlan.length">
          <td colspan="5" style="text-align: center;">Keine Calcium-Dosierungen geplant</td>
        </tr>
        <tr v-for="(entry, index) in caDosagePlan" :key="'ca-'+index">
          <td>{{ entry.formattedDate }}</td>
          <td style="text-align: right;">{{ entry.caDosage !== null && entry.caDosage !== undefined ? entry.caDosage.toFixed(2) : '-' }}</td>
          <td style="text-align: right;">{{ entry.mgDosage !== null && entry.mgDosage !== undefined ? entry.mgDosage.toFixed(2) : '-' }}</td>
          <td :class="entry.projectedCa !== null ? getValueHighlightClass(entry.projectedCa, systemSettings.targetCalcium, 20, 40) : ''" style="text-align: right;">
            {{ entry.projectedCa !== null ? entry.projectedCa.toFixed(0) : '-' }}
          </td>
          <td>{{ entry.type }}</td>
        </tr>
      </tbody>
    </table>
  </div>
</div>
<div class="card">
  <div class="card-header" style="display: flex; justify-content: space-between; align-items: center;">
    <h2 class="card-title">Dosierungshistorie</h2>
    <div class="day-navigation">
      <button class="nav-btn" @click="navigateHistoryDay(-1)" :disabled="currentHistoryDayOffset <= 0" title="Vorheriger Tag">&lt;</button>
      <span style="margin: 0 10px; font-weight: 600;">{{ dosageHistoryDayTitle }}</span>
      <button class="nav-btn" @click="navigateHistoryDay(1)" :disabled="currentHistoryDayOffset >= historyDays.length - 1" title="Nächster Tag">&gt;</button>
    </div>
  </div>

  <!-- Hinweis zur Navigation -->
  <div v-if="historyDays.length > 0" class="message-box info" style="margin: 10px 0; text-align: center; padding: 8px; background-color: rgba(3, 169, 244, 0.1);">
    <span style="font-size: 0.9rem;">Tag {{ currentHistoryDayOffset + 1 }} von {{ historyDays.length }}</span>
  </div>
  <div v-else class="message-box info" style="margin: 10px 0; text-align: center; padding: 8px; background-color: rgba(3, 169, 244, 0.1);">
    <span style="font-size: 0.9rem;">Keine Dosierungshistorie verfügbar</span>
  </div>
               <div class="table-container">
                  <table class="dosage-history-table">
                     <thead>
                        <tr>
                           <th>Datum</th>
                           <th>Pumpe</th>
                           <th>Menge (ml)</th>
                           <th>Faktor</th>
                           <th>Typ</th>
                           <th>Modus</th>
                           <th>Aktion</th>
                        </tr>
                     </thead>
                     <tbody>
                        <tr v-if="!filteredDosageHistory.length">
                           <td colspan="7" style="text-align: center;">Keine Dosierungen für diesen Tag</td>
                        </tr>
                        <tr v-for="dosage in filteredDosageHistory" :key="dosage.index">
                           <td>{{ dosage.date }}</td>
                           <td>{{ dosage.pumpName }}</td>
                           <td style="text-align: right;">{{ dosage.amount.toFixed(2) }} ml</td>
                           <td style="text-align: center;">{{ dosage.factor != null ? dosage.factor.toFixed(2) + 'x' : '1.00x' }}</td>
                           <td>{{ dosage.typeName }}</td>
                           <td>{{ dosage.isAutomatic ? 'Automatisch' : 'Manuell' }}</td>
                           <td>
                              <span class="delete-btn action-button" @click="deleteDosage(dosage.index, dosage.dosageType)">✕</span>
                           </td>
                        </tr>
                     </tbody>
                  </table>
               </div>
            </div>
         </div>
         <!-- Manuelle Steuerung Tab -->
         <div id="manualTab" class="tab-content" :class="{ active: activeTab === 'manual' }">
            <div class="card">
               <div class="card-header">
                  <h2 class="card-title">Dosierpumpen Status</h2>
               </div>
               <div class="table-container">
                  <table>
                     <thead>
                        <tr>
                           <th>ID</th>
                           <th>Name</th>
                           <th>Kalibrierung (Schritte/ml)</th>
                           <th>Letzte Kalibrierung</th>
                           <th>Geschwindigkeit (ml/min)</th>
                           <th>Beschleunigung (ml/min²)</th>
                        </tr>
                     </thead>
                     <tbody>
                        <tr v-for="pump in pumps" :key="pump.id">
                           <td>{{ pump.id }}</td>
                           <td>{{ pump.name }}</td>
                           <td>{{ pump.isCalibrated ? (1 / pump.mlPerStep).toFixed(2) + ' Schritte/ml' : 'Nicht kalibriert' }}</td>
                           <td>{{ pump.lastCalibration }}</td>
                           <td>{{ pump.speedML ? pump.speedML.toFixed(2) : '0.00' }}</td>
                           <td>{{ pump.accelerationML > 0 ? pump.accelerationML.toFixed(2) : 'Deaktiviert' }}</td>
                        </tr>
                     </tbody>
                  </table>
               </div>
            </div>
            <div class="card">
               <div class="card-header">
                  <h2 class="card-title">Flüssigkeit Dosieren</h2>
               </div>
               <div v-if="lastDispenseMessage" class="message-box success">{{ lastDispenseMessage }}</div>
               <div class="form-row">
                  <div class="form-column">
                     <div class="form-group">
                        <label for="dispensePump">Pumpe auswählen:</label>
                        <select id="dispensePump" v-model="dispense.pumpIndex">
                           <option v-for="pump in pumps" :key="pump.id" :value="pump.id" 
                              :disabled="!pump.isCalibrated">
                              {{ pump.name }} {{ pump.isCalibrated ? 
                              `(${(1/pump.mlPerStep).toFixed(2)} Schritte/ml)` : 
                              '(Nicht kalibriert)' }}
                           </option>
                        </select>
                     </div>
                  </div>
                  <div class="form-column">
                     <div class="form-group">
                        <label for="dispenseMl">Zu dosierende Menge (ml):</label>
                        <input type="number" id="dispenseMl" v-model="dispense.ml" step="0.1" min="0" required>
                     </div>
                  </div>
               </div>
               <div class="form-group">
                  <button @click="startDispense">Dosierung starten</button>
               </div>
               <div v-if="dispensingProgress.show" class="progress-container" style="margin-top: 15px;">
                  <div class="progress-label">Dosierung läuft...</div>
                  <div class="progress-container">
                     <div class="progress-bar" :style="{ width: dispensingProgress.percent + '%' }"></div>
                  </div>
                  <div class="progress-info">
                     <span>{{ dispensingProgress.percent }}%</span>
                     <span>{{ dispensingProgress.currentSteps }} / {{ dispensingProgress.totalSteps }} Schritte</span>
                  </div>
               </div>
            </div>
            <div class="card">
               <div class="card-header">
                  <h2 class="card-title">Pumpe Kalibrieren</h2>
               </div>
               <div v-if="!calibration.running">
                  <div class="form-row">
                     <div class="form-column">
                        <div class="form-group">
                           <label for="calibrationPump">Pumpe auswählen:</label>
                           <select id="calibrationPump" v-model="calibration.pumpIndex">
                              <option v-for="pump in pumps" :key="pump.id" :value="pump.id">
                                 {{ pump.name }}
                              </option>
                           </select>
                        </div>
                     </div>
                     <div class="form-column">
                        <div class="form-group">
                           <label for="calibrationSteps">Kalibrierungsschritte:</label>
                           <input type="number" id="calibrationSteps" v-model="calibration.steps" min="1">
                        </div>
                     </div>
                  </div>
                  <div class="form-group">
                     <button @click="startCalibration">Kalibrierung starten</button>
                  </div>
               </div>
               <div v-else>
                  <div class="message-box info">
                     Kalibrierung für {{ getPumpName(currentCalibrationPump) }} läuft. 
                     {{ currentCalibrationSteps }} Schritte wurden gefahren.
                  </div>
                  <div class="form-group">
                     <label for="calibrationMl">Gemessene Flüssigkeitsmenge (ml):</label>
                     <input type="number" id="calibrationMl" v-model="calibration.ml" step="0.01" min="0" required>
                  </div>
                  <div class="form-group">
                     <button @click="saveCalibration">Kalibrierung speichern</button>
                  </div>
               </div>
               <div v-if="calibrationProgress.show" class="progress-container" style="margin-top: 15px;">
                  <div class="progress-label">Kalibrierung läuft...</div>
                  <div class="progress-container">
                     <div class="progress-bar" :style="{ width: calibrationProgress.percent + '%' }"></div>
                  </div>
                  <div class="progress-info">
                     <span>{{ calibrationProgress.percent }}%</span>
                     <span>{{ calibrationProgress.currentSteps }} / {{ calibrationProgress.totalSteps }} Schritte</span>
                  </div>
               </div>
            </div>
            <div class="card">
               <div class="card-header">
                  <h2 class="card-title">Globale Pumpeneinstellungen</h2>
               </div>
               <div class="form-row">
                  <div class="form-column">
                     <div class="form-group">
                        <label for="speedML">Geschwindigkeit für alle Pumpen (ml/min):</label>
                        <input type="number" id="speedML" v-model="globalSettings.speedML" min="0.01" step="0.01">
                     </div>
                  </div>
                  <div class="form-column">
                     <div class="form-group">
                        <label for="accelerationML">Beschleunigung für alle Pumpen (ml/min², 0 = deaktiviert):</label>
                        <input type="number" id="accelerationML" v-model="globalSettings.accelerationML" min="0" step="0.01">
                     </div>
                  </div>
               </div>
               <div class="form-group">
                  <button @click="setGlobalSettings">Einstellungen anwenden</button>
               </div>
            </div>
         </div>
         <!-- Behälter Tab -->
         <div id="containerTab" class="tab-content" :class="{ active: activeTab === 'container' }">
            <div class="card">
               <div class="card-header">
                  <h2 class="card-title">Behälter-Überwachung</h2>
               </div>
               <p style="margin-bottom: 20px;">Überwachung der Füllstände aller Flüssigkeitsbehälter</p>
               <div class="table-container">
                  <table>
                     <thead>
                        <tr>
                           <th>Name</th>
                           <th>Kapazität (ml)</th>
                           <th>Aktion</th>
                           <th>Füllstand</th>
                           <th>Verbleibende Tage</th>
                        </tr>
                     </thead>
                     <tbody>
                        <tr v-for="container in systemSettings.containers" :key="container.id">
                           <td>{{ container.name }}</td>
<td>
    <input type="number" v-model.number="container.capacity" min="1" max="50000" step="100" style="width: 100px;" 
           @change="saveContainerCapacity(container.id, container.capacity)">
</td>
                           <td>
                              <button @click="refillContainer(container.id)">Nachfüllen</button>
                           </td>
                           <td>
                              <div class="progress-container">
                                 <div class="progress-bar" 
                                    :style="{ width: container.percentage + '%', 
                                    backgroundColor: getContainerColor(container.percentage) }">
                                 </div>
                                 <div class="progress-text">
                                    {{ Math.round(container.level) }}ml / {{ Math.round(container.capacity) }}ml
                                 </div>
                              </div>
                           </td>
                           <td v-html="formatDaysRemaining(container.daysRemaining)"></td>
                        </tr>
                     </tbody>
                  </table>
               </div>
            </div>
         </div>

<!-- Einstellungen Tab -->
<div id="settingsTab" class="tab-content" :class="{ active: activeTab === 'settings' }">
   <div class="card">
      <div class="card-header">
         <h2 class="card-title">Systemeinstellungen</h2>
      </div>
      <div style="background-color: #e8f5e9; border-left: 4px solid #4CAF50; padding: 12px; margin-bottom: 20px; border-radius: 4px;">
         <p style="margin: 0; color: #2e7d32; font-size: 14px;">
            <strong>ℹ️ Automatisches Speichern:</strong> Ihre Änderungen werden automatisch gespeichert, sobald Sie ein Eingabefeld verlassen.
         </p>
      </div>
      <div class="form-group">
         <label for="aquariumVolume">Aquariumvolumen (Liter):</label>
         <input type="number" id="aquariumVolume" v-model="systemSettings.aquariumVolume" @blur="saveSettings" step="1" min="10" max="5000">
      </div>
      <h3 style="margin: 20px 0 15px 0;">Zielwerte</h3>
      <div class="form-row">
         <div class="form-column">
            <div class="form-group">
               <label for="targetKH">Ziel-KH (°dKH):</label>
               <input type="number" id="targetKH" v-model="systemSettings.targetKH" @blur="saveSettings" step="0.1" min="0" max="20">
            </div>
         </div>
         <div class="form-column">
            <div class="form-group">
               <label for="targetCalcium">Ziel-Calcium (mg/l):</label>
               <input type="number" id="targetCalcium" v-model="systemSettings.targetCalcium" @blur="saveSettings" step="1" min="0" max="1000">
            </div>
         </div>
      </div>
      <h3 style="margin: 20px 0 15px 0;">Dosierlimits</h3>
      <div class="form-row">
         <div class="form-column">
            <div class="form-group">
               <label for="maxDailyChangeKH">Maximale tägliche KH-Änderung (°dKH):</label>
               <input type="number" id="maxDailyChangeKH" v-model="systemSettings.maxDailyChangeKH" @blur="saveSettings" step="0.1" min="0" max="3">
            </div>
         </div>
         <div class="form-column">
            <div class="form-group">
               <label for="maxDailyChangeCalcium">Maximale tägliche Calcium-Änderung (mg/l):</label>
               <input type="number" id="maxDailyChangeCalcium" v-model="systemSettings.maxDailyChangeCalcium" @blur="saveSettings" step="1" min="0" max="30">
            </div>
         </div>
      </div>
      <h3 style="margin: 20px 0 15px 0;">Berechnungseinstellungen</h3>
      <div class="form-row">
         <div class="form-column">
            <div class="form-group">
               <label for="historyCount">Anzahl der historischen Messwerte für Berechnung:</label>
               <input type="number" id="historyCount" v-model="systemSettings.historyCount" @blur="saveSettings" step="1" min="1" max="4">
               <span class="help-text">
                  <strong>Empfehlung:</strong> 3-4 Messungen für eine ausgewogene Verbrauchsanalyse.<br>
                  Alle Messintervalle werden gleichgewichtet berücksichtigt.
               </span>
            </div>
         </div>
         <div class="form-column">
<!-- weightingMethod UI-Element komplett entfernt -->
         </div>
      </div>
               <h3 style="margin: 20px 0 15px 0;">Dosierverhältnisse</h3>
               <div class="form-group">
                  <label for="magnesiumRatio">Magnesium-Calcium Verhältnis (%):</label>
                  <input type="number" id="magnesiumRatio" v-model="systemSettings.magnesiumRatio" @blur="saveSettings" step="1" min="0" max="100">
                  <span class="help-text">Prozentsatz der Calcium-Dosierung, der für Magnesium verwendet wird</span>
               </div>
               <h3 style="margin: 20px 0 15px 0;">KH-Nacht-Dosierung</h3>
               <div class="form-row">
                  <div class="form-column">
                     <div class="form-group">
                        <label for="khNightStart">KH-Nacht Startzeit (Stunde):</label>
                        <input type="number" id="khNightStart" v-model="systemSettings.khNightStart" @blur="saveSettings" step="1" min="0" max="23">
                        <span class="help-text">Startzeit der KH-Nacht-Dosierung (0-23 Uhr)</span>
                     </div>
                  </div>
                  <div class="form-column">
                     <div class="form-group">
                        <label for="khNightEnd">KH-Nacht Endzeit (Stunde):</label>
                        <input type="number" id="khNightEnd" v-model="systemSettings.khNightEnd" @blur="saveSettings" step="1" min="0" max="23">
                        <span class="help-text">Endzeit der KH-Nacht-Dosierung (0-23 Uhr)</span>
                     </div>
                  </div>
               </div>
               <!-- Neue Einstellung für pH-basierte KH-Dosierung mit verbessertem Layout -->
               <div class="form-row">
                  <div class="form-column">
                     <div class="form-group" style="display: flex; align-items: center; gap: 15px;">
                        <label for="usePhBasedKHDosing" style="flex: 1; min-width: 220px;">pH-basierte KH-Dosierung aktivieren:</label>
                        <label class="switch" style="margin-right: 15px;">
                        <input type="checkbox" id="usePhBasedKHDosing" v-model="systemSettings.usePhBasedKHDosing" @change="saveSettings">
                        <span class="slider"></span>
                        </label>
                     </div>
                     <span class="help-text" style="margin-top: 8px;">Bei Aktivierung wird KH-Nacht dosiert, wenn der pH-Wert unter dem Schwellwert liegt</span>
                  </div>
                  <div class="form-column">
                     <div class="form-group">
                        <label for="phThresholdForKHNight">pH-Schwellwert für KH-Nacht:</label>
                        <input type="number" id="phThresholdForKHNight" v-model="systemSettings.phThresholdForKHNight" @blur="saveSettings" step="0.1" min="6.0" max="9.0">
                        <span class="help-text">KH-Nacht wird dosiert, wenn pH unter diesem Wert liegt</span>
                     </div>
                  </div>
               </div>
               <h3 style="margin: 20px 0 15px 0;">Initiale Verbrauchsraten</h3>
               <p class="help-text" style="margin-bottom: 15px;">Diese Werte werden verwendet, wenn noch keine ausreichenden Messdaten vorliegen, um den Verbrauch zu berechnen.</p>
               <div class="form-row">
                  <div class="form-column">
                     <div class="form-group" style="display: flex; align-items: center; gap: 15px;">
                        <label for="autoUpdateInitialRates">Initiale Raten automatisch aktualisieren:</label>
                        <label class="switch">
                        <input type="checkbox" id="autoUpdateInitialRates" v-model="systemSettings.autoUpdateInitialRates" @change="saveSettings">
                        <span class="slider"></span>
                        </label>
                     </div>
                     <span class="help-text" style="margin-top: 8px;">Bei jeder Messung werden die initialen Raten mit den berechneten Werten überschrieben.</span>
                  </div>
               </div>
               <div class="form-row">
                  <div class="form-column">
                     <div class="form-group">
                        <label for="initialKHConsumption">Initiale KH-Verbrauchsrate (ml/Tag):</label>
                        <input type="number" id="initialKHConsumption" v-model="systemSettings.initialKHConsumption" @blur="saveSettings" step="0.1" min="0" max="500">
                        <span class="help-text">Verbrauch in ml/Tag KH-Lösung</span>
                     </div>
                  </div>
                  <div class="form-column">
                     <div class="form-group">
                        <label for="initialCalciumConsumption">Initiale Calcium-Verbrauchsrate (ml/Tag):</label>
                        <input type="number" id="initialCalciumConsumption" v-model="systemSettings.initialCalciumConsumption" @blur="saveSettings" step="0.1" min="0" max="500">
                        <span class="help-text">Verbrauch in ml/Tag Calcium-Lösung</span>
                     </div>
                  </div>
               </div>
               <h3 style="margin: 20px 0 15px 0;">Anti-Tropf-Mechanismus</h3>
               <p class="help-text" style="margin-bottom: 15px;">Diese Einstellungen verhindern Tropfenbildung am Schlauchende durch kurzen Rückzug nach der Dosierung.</p>
               <div class="form-row">
                  <div class="form-column">
                     <div class="form-group" style="display: flex; align-items: center; gap: 15px;">
                        <label for="enableAntiDrip" style="flex: 1; min-width: 220px;">Anti-Tropf-Funktion aktivieren:</label>
                        <label class="switch" style="margin-right: 15px;">
                        <input type="checkbox" id="enableAntiDrip" v-model="systemSettings.enableAntiDrip" @change="saveSettings">
                        <span class="slider"></span>
                        </label>
                     </div>
                     <span class="help-text" style="margin-top: 8px;">Aktiviert den Rückzug nach Dosierung</span>
                  </div>
                  <div class="form-column">
<div class="form-group">
   <label for="antiDripML">Rückzugsvolumen (ml):</label>
   <input type="number" id="antiDripML" v-model="systemSettings.antiDripML" @blur="saveSettings" step="0.01">
   <span class="help-text">Volumen in ml für den Rückzug</span>
</div>
                  </div>
               </div>
               <div class="form-row">
                  <div class="form-column">
<div class="form-group">
   <label for="antiDripSpeedML">Rückzugsgeschwindigkeit (ml/min):</label>
   <input type="number" id="antiDripSpeedML" v-model="systemSettings.antiDripSpeedML" @blur="saveSettings" step="0.01">
   <span class="help-text">Geschwindigkeit des Rückzugs in ml/min</span>
</div>
                  </div>
               </div>
               
               <!-- Neue Zeiteinstellungen-Sektion -->
               <h3 style="margin: 20px 0 15px 0;">Zeiteinstellungen</h3>
               <div class="form-row">
                  <div class="form-column">
                     <div class="form-group">
                        <label for="timeOffset">Zeit-Offset (Sekunden):</label>
                        <input type="number" id="timeOffset" v-model="systemSettings.timeOffset" @blur="saveSettings" step="3600" min="-43200" max="43200">
                        <span class="help-text">Offset zur UTC-Zeit in Sekunden (3600 = +1h, 7200 = +2h)</span>
                     </div>
                  </div>
               </div>

               <!-- WLAN-Einstellungen-Sektion -->
               <h3 style="margin: 20px 0 15px 0;">WLAN-Einstellungen</h3>
               <div class="info-box" v-if="wifiConfig.isAPMode" style="background: #fff3cd; border-color: #ffc107; margin-bottom: 15px;">
                  <p><strong>⚠️ Access Point Modus aktiv!</strong></p>
                  <p>Verbinde dich mit einem WLAN, um das System online zu bringen.</p>
               </div>
               <div class="info-box" v-else-if="wifiConfig.configured" style="background: #d4edda; border-color: #28a745; margin-bottom: 15px;">
                  <p><strong>✓ Verbunden mit: {{ wifiConfig.ssid }}</strong></p>
                  <p v-if="wifiConfig.ipAddress">IP-Adresse: {{ wifiConfig.ipAddress }}</p>
               </div>
               <div class="form-row">
                  <div class="form-column">
                     <div class="form-group">
                        <label for="wifiSSID">WLAN-Name (SSID):</label>
                        <input type="text" id="wifiSSID" v-model="wifiSettings.ssid" placeholder="Dein WLAN-Name">
                        <span class="help-text">Name deines WLAN-Netzwerks</span>
                     </div>
                  </div>
                  <div class="form-column">
                     <div class="form-group">
                        <label for="wifiPassword">WLAN-Passwort:</label>
                        <input type="password" id="wifiPassword" v-model="wifiSettings.password" placeholder="WLAN-Passwort">
                        <span class="help-text">Passwort für dein WLAN</span>
                     </div>
                  </div>
               </div>
               <div class="form-group">
                  <button @click="saveWiFiConfig" class="primary">WLAN-Einstellungen speichern & neu starten</button>
               </div>

<div class="form-group" style="display: flex; gap: 10px; margin-top: 20px;">
                  <button @click="resetSettings" class="danger">Einstellungen zurücksetzen</button>
               </div>
               <!-- Backup-Funktion entfernt -->
            </div>
         </div>
         <!-- pH-Wert-Tab -->
         <div id="phTab" class="tab-content" :class="{ active: activeTab === 'ph' }">
            <div class="card">
               <div class="card-header">
                  <h2 class="card-title">pH-Wert Messung</h2>
               </div>
               <div class="form-row">
                  <div class="form-column">
                     <div class="form-group">
                        <h3>Aktueller pH-Wert</h3>
                        <div class="widget-value" :class="getPhValueHighlightClass(phData.currentPH)">
                           {{ phData.currentPH }}
                        </div>
                        <div class="widget-label">Konstante Temperatur: {{ phData.fixedTemperature || '25.0' }}°C</div>
                     </div>
                  </div>
                  <div class="form-column">
                     <div class="form-group">
                        <button @click="measurePh" style="margin-top: 20px;">Jetzt messen</button>
                     </div>
                  </div>
               </div>
               <div v-if="phData.isCalibrating || phData.isCalibrationStable" class="message-box info">
                  {{ phData.isCalibrationStable ? 
                  "pH-Kalibrierung erfolgreich abgeschlossen!" : 
                  "Kalibrierung läuft...bitte warten, bis der Messwert stabil ist." }}
               </div>
               <div v-if="phData.isCalibrating" class="calibration-progress">
                  <div class="progress-label">Kalibrierung läuft...</div>
                  <div class="progress-container">
                     <div class="progress-bar" :style="{ width: phCalibrationProgress + '%' }"></div>
                  </div>
                  <div class="progress-info">
                     <span>{{ phData.rawVoltage ? phData.rawVoltage.toFixed(1) + ' mV' : '0 mV' }}</span>
                  </div>
               </div>
               <div class="card-header" style="margin-top: 20px;">
                  <h2 class="card-title">pH-Sensor kalibrieren</h2>
               </div>
               <div class="form-row">
                  <div class="form-column">
                     <div class="form-group">
                        <p>Tauchen Sie die pH-Sonde in die entsprechende Kalibrierlösung und klicken Sie dann auf den entsprechenden Kalibrierungsknopf.</p>
                        <p>Die Kalibrierung wird automatisch durchgeführt, sobald sich der Messwert stabilisiert hat.</p>
                     </div>
                  </div>
               </div>
               <div class="form-row">
                  <div class="form-column">
                     <div class="form-group">
                        <button @click="calibratePh(4)" style="margin-top: 10px;">Kalibrieren mit pH 4,0</button>
                     </div>
                  </div>
                  <div class="form-column">
                     <div class="form-group">
                        <button @click="calibratePh(7)" style="margin-top: 10px;">Kalibrieren mit pH 7,0</button>
                        <div class="voltage-display">
                           Spannung: {{ phData.rawVoltage ? phData.rawVoltage.toFixed(1) + ' mV' : '-- mV' }}
                           <div class="calibration-info">
                              <small class="help-text">Letzte pH 7 Kalibrierung: {{ phData.phCalibration?.formattedTime_pH7 || '--' }}</small><br>
                              <small class="help-text">Letzte pH 4 Kalibrierung: {{ phData.phCalibration?.formattedTime_pH4 || '--' }}</small>
                           </div>
                        </div>
                     </div>
                  </div>
               </div>
            </div>
            <div class="card">
               <div class="card-header" style="display: flex; justify-content: space-between; align-items: center;">
                  <h2 class="card-title">pH-Messwerte Verlauf</h2>
                  <div class="day-navigation">
                     <button class="nav-btn" @click="navigatePhHistory(-1)" :disabled="currentPhHistoryDayOffset <= -1">&lt;</button>
                     <span>{{ phHistoryDayTitle }}</span>
                     <button class="nav-btn" @click="navigatePhHistory(1)" :disabled="currentPhHistoryDayOffset >= phHistoryDays.length - 1">&gt;</button>
                  </div>
               </div>
               <div class="table-container">
                  <table>
                     <thead>
                        <tr>
                           <th>Datum</th>
                           <th>pH-Wert</th>
                           <th>Aktion</th>
                        </tr>
                     </thead>
                     <tbody>
                        <tr v-if="!filteredPhMeasurements.length">
                           <td colspan="3" style="text-align: center;">Keine pH-Messungen für diesen Tag</td>
                        </tr>
                        <tr v-for="measurement in filteredPhMeasurements" :key="measurement.index">
                           <td>{{ measurement.date }}</td>
                           <td>{{ measurement.value.toFixed(2) }}</td>
                           <td>
                              <span class="delete-btn action-button" @click="deletePhMeasurement(measurement.index)">✕</span>
                           </td>
                        </tr>
                     </tbody>
                  </table>
               </div>
            </div>
         </div>
      </div>
      <script>
// Warte, bis das DOM vollständig geladen ist
document.addEventListener('DOMContentLoaded', function() {
  // Create Vue application
  const app = Vue.createApp({
data() {
  return {
    // Global data
    connected: false,
    timeInitialized: false,
    currentTime: '--:--:--',
    timeSource: 'unbekannt',  // 'RTC' oder 'TimeLib'
    activeTab: 'dashboard',
    message: {
      text: '',
      type: 'info'
    },
    socket: null,
    _autoDosingLockUntil: 0,  // Optimistic Lock für Toggle (Zeitstempel bis wann Lock aktiv)
            
    // Tab definitions
tabs: [
  { id: 'dashboard', name: 'Dashboard' },
  { id: 'kalkhaushalt', name: 'Wasserparameter' },
  { id: 'dosage', name: 'Dosiersystem' },
  { id: 'manual', name: 'Manuelle Steuerung' },
  { id: 'container', name: 'Behälter' },
  { id: 'ph', name: 'pH-Wert' },
  { id: 'settings', name: 'Einstellungen' }
],
    
    // Pumps data
    pumps: [],
    currentCalibrationPump: -1,
    currentCalibrationSteps: 0,
    lastPumpOperation: -1,
    lastPumpAmount: 0,
    activePumpIndex: -1,
    targetSteps: 0,
    isCalibrationRunning: false,
    isDispensingRunning: false,
    
collapsible: {
      khMeasurements: true,
      caMeasurements: true,
      autoKhMeasurements: true  // NEU: Automatische KH-Messungen
    },
    
    // System settings
    systemSettings: {
             aquariumVolume: 450,
             targetKH: 7.5,
             targetCalcium: 420,
             historyCount: 5,
             autoDosing: false,
             maxDailyChangeKH: 2.0,
             maxDailyChangeCalcium: 20.0,
             lastAutoDosage: 0,
             lastAutoDosageFormatted: '',
             magnesiumRatio: 50.0,
             khNightStart: 19,
             khNightEnd: 7,
             initialKHConsumption: 160,
             initialCalciumConsumption: 60,
             autoUpdateInitialRates: true,
             dailyKHConsumption: 0,
             dailyCalciumConsumption: 0,
             nextKHDosage: 0,
             nextCalciumDosage: 0,
             currentKH: 0,
             currentCalcium: 0,
             containers: [],
             usePhBasedKHDosing: false,
             phThresholdForKHNight: 8.0,
             enableAntiDrip: true,
             antiDripML: 0.015,
             antiDripSpeedML: 3.6,
             timeOffset: 3600
         },

         // Dispense form data
         dispense: {
             pumpIndex: 0,
             ml: 1
         },

         // Calibration form data
         calibration: {
             pumpIndex: 0,
             steps: 1000,
             ml: 0,
             running: false
         },

         // Global pump settings
         globalSettings: {
             speedML: 3.6,
             accelerationML: 1.8
         },
         
         // Progress indicators
         dispensingProgress: {
             show: false,
             percent: 0,
             currentSteps: 0,
             totalSteps: 0
         },
         calibrationProgress: {
             show: false,
             percent: 0,
             currentSteps: 0,
             totalSteps: 0
         },
         
         // New water measurement form
         newMeasurement: {
             kh: '',
             calcium: ''
         },
         
         // Water measurements data
         waterMeasurements: {
             kh: [],
             calcium: [],
             autoKh: []  // NEU: Automatische KH-Messungen
         },

         // Schwankungskompensation
         dosingFactors: {
             enabled: false,
             factors: [1,1,1,1,1,1,1,1,1,1,1,1],
             hasNewPattern: false
         },
                  
         // Dosage plan
         dosagePlan: [],
         dosagePlanDayOffset: 0,
         maxDosageDays: 0,
         dosagePlanDayTitle: 'Heute',

             // Neue Eigenschaften
    khDosagePlan: [],
    caDosagePlan: [],
         
         // Dosage history
         dosageHistory: [],
historyDays: [],                // Sammlung von Tagen mit Dosierungshistorie
currentHistoryDayOffset: 0,     // Aktueller Tag-Offset für die Historie
dosageHistoryDayTitle: "Alle Dosierungen",  // Titel für aktuellen Tag
                  
         // pH Data
         phData: {
             currentPH: 7.0,
             fixedTemperature: 25.0,
             rawVoltage: 0,
             isCalibrating: false,
             isCalibrationStable: false,
             phCalibration: {
                 ph4Voltage: 0,
                 ph7Voltage: 0,
                 isCalibrated: false,
                 formattedTime_pH4: 'Nie',
                 formattedTime_pH7: 'Nie'
             },
             ph: []
         },
         phHistoryDays: [],
         currentPhHistoryDayOffset: 0,
         phHistoryDayTitle: 'Alle Messungen',
phCalibrationProgress: 0,
         
// Consumption chart data
         consumptionData: [],
         
// pH trend chart data
phTrendData: [],

// Messages
DispenseMessage: '',
            
            // Speicher-Status
            memoryStatus: null,

            // WiFi-Konfiguration
            wifiSettings: {
               ssid: '',
               password: ''
            },
            wifiConfig: {
               ssid: '',
               configured: false,
               isAPMode: false,
               ipAddress: ''
            }

         };
         },
computed: {
// Get filtered dosage history for current day
filteredDosageHistory() {
  // Einfach die Dosierungshistorie zurückgeben, da wir vom Server 
  // bereits nur den ausgewählten Tag erhalten
  return this.dosageHistory;
},
    
// Get filtered pH measurements for current day
    filteredPhMeasurements() {
        if (this.currentPhHistoryDayOffset === -1 || !this.phHistoryDays.length) {
            return this.phData.ph || [];
        }
        
        const selectedDay = this.phHistoryDays[this.currentPhHistoryDayOffset];
        return (this.phData.ph || []).filter(measurement => measurement.date.startsWith(selectedDay));
    }
},
methods: {


setActiveTab(tabId) {
    this.activeTab = tabId;

    // Load tab-specific data
    switch(tabId) {
case 'dashboard':
            // Anfragen GESTAFFELT senden — ESP32 kann nicht alle gleichzeitig verarbeiten
            this.getSystemSettings();
            setTimeout(() => this.getAllKhDosagePlan(), 300);
            setTimeout(() => this.getAllCaDosagePlan(), 600);
            setTimeout(() => this.getWaterMeasurements(), 900);
            setTimeout(() => this.loadConsumptionData(), 1200);
            setTimeout(() => this.loadPhTrendData(), 1500);
            break;
        case 'kalkhaushalt':
            this.getAllWaterMeasurements();
            break;
case 'dosage':
    this.getSystemSettings();
    setTimeout(() => this.getAllKhDosagePlan(), 300);
    setTimeout(() => this.getAllCaDosagePlan(), 600);
    setTimeout(() => {
        this.currentHistoryDayOffset = 0;
        this.historyDays = [];
        this.getDosageHistory();
    }, 900);
    break;
                         case 'manual':
                             this.getStatus();
                             break;
                         case 'container':
                             this.getSystemSettings();
                             break;
                         case 'ph':
                             this.getPhMeasurements();
                             this.startVoltageMonitoring();
                             break;
case 'settings':
    this.getSystemSettings();
    break;
                     }

                     // Stop voltage monitoring when leaving pH tab
                     if (tabId !== 'ph' && this.voltageMonitoringInterval) {
                         this.stopVoltageMonitoring();
                     }
                 },
                 
                 // Neue Methoden für vollständige Dosierpläne
                 getAllKhDosagePlan() {
                     this.sendMessage({
                         action: 'getAllKhDosagePlan'
                     });
                 },
                 
                 getAllCaDosagePlan() {
                     this.sendMessage({
                         action: 'getAllCaDosagePlan'
                     });
                 },
                 
                 // Connect to WebSocket
                 connectWebSocket() {
                     // IP-Adresse und Port des ESP32 WebSocket-Servers
                     const wsUrl = `ws://${window.location.hostname}/ws`;
                     console.log(`Versuche WebSocket-Verbindung zu: ${wsUrl}`);
                     
                     // Falls eine alte Verbindung existiert, diese zuerst schließen
                     if (this.socket && this.socket.readyState !== WebSocket.CLOSED) {
                         try {
                             this.socket.close();
                         } catch (e) {
                             console.error("Fehler beim Schließen des alten Sockets:", e);
                         }
                     }
                     
                     try {
                         // Neue Verbindung aufbauen
                         this.socket = new WebSocket(wsUrl);
                         
                         // Timeout für die Verbindungsherstellung
                         const connectionTimeout = setTimeout(() => {
                             if (this.socket && this.socket.readyState !== WebSocket.OPEN) {
                                 console.error("WebSocket-Verbindung Timeout");
                                 try {
                                     this.socket.close();
                                 } catch (e) {
                                     console.error("Fehler beim Schließen des Sockets:", e);
                                 }
                                 this.showMessage('Verbindungstimeout. Versuche erneut zu verbinden...', 'error');
                                 setTimeout(() => this.connectWebSocket(), 5000);
                             }
                         }, 5000);
                         
                         this.socket.onopen = () => {
                             clearTimeout(connectionTimeout);
                             console.log("WebSocket-Verbindung erfolgreich hergestellt");
                             this.connected = true;
                             this.wsReconnectDelay = 5000;  // Reset bei Erfolg

                             // Init-Daten erst nach kurzer Pause anfordern,
                             // damit die Verbindung sich stabilisieren kann
                             setTimeout(() => {
                                 if (this.socket && this.socket.readyState === WebSocket.OPEN) {
                                     this.sendMessage({action: 'init'});
                                 }
                             }, 200);

                             // Zeit erst nach init anfordern
                             setTimeout(() => {
                                 if (this.socket && this.socket.readyState === WebSocket.OPEN) {
                                     this.sendMessage({action: 'getCurrentTime'});
                                     this.checkTimeSync();
                                 }
                             }, 1500);

                         };

                         this.socket.onclose = (event) => {
                             console.log("WebSocket geschlossen mit Code:", event.code, "Grund:", event.reason);
                             this.connected = false;
                             const delay = this.wsReconnectDelay || 5000;
                             // Exponential Backoff: 5s -> 10s -> 20s -> max 30s
                             this.wsReconnectDelay = Math.min((this.wsReconnectDelay || 5000) * 2, 30000);
                             setTimeout(() => this.connectWebSocket(), delay);
                         };

                         this.socket.onerror = (error) => {
                             console.error('WebSocket Fehler aufgetreten:', error);
                             this.connected = false;
                             // Nicht close() aufrufen — onclose wird automatisch ausgelöst
                         };

this.socket.onmessage = (event) => {
    try {
        const raw = event.data;
        // Chunked-Protokoll: "CHUNK:<id>:<seq>:<total>:<payload>"
        // ESP sendet große JSON-Payloads (pH-/Wassermessungen) in kleinen
        // Frames, weil die AsyncWebSocket-Library sonst riesige Heap-Buffer
        // anlegen müsste und der ESP32 OOM geht.
        if (typeof raw === 'string' && raw.startsWith('CHUNK:')) {
            // 4 Doppelpunkte → 5 Teile, ab dem 5. beginnt die Payload
            // (die Payload selbst kann ':' enthalten, deshalb manuell parsen)
            let p1 = raw.indexOf(':', 6);                   // nach "CHUNK:"
            let p2 = p1 >= 0 ? raw.indexOf(':', p1 + 1) : -1;
            let p3 = p2 >= 0 ? raw.indexOf(':', p2 + 1) : -1;
            if (p1 < 0 || p2 < 0 || p3 < 0) {
                console.warn('Ungültiger CHUNK-Frame:', raw.slice(0, 80));
                return;
            }
            const id    = raw.substring(6, p1);
            const seq   = parseInt(raw.substring(p1 + 1, p2), 10);
            const total = parseInt(raw.substring(p2 + 1, p3), 10);
            const payload = raw.substring(p3 + 1);

            if (!this._chunkBuffers) this._chunkBuffers = {};
            let entry = this._chunkBuffers[id];
            if (!entry || entry.total !== total) {
                entry = { total, received: 0, parts: new Array(total) };
                this._chunkBuffers[id] = entry;
            }
            if (entry.parts[seq] === undefined) {
                entry.parts[seq] = payload;
                entry.received++;
            }
            if (entry.received === entry.total) {
                const full = entry.parts.join('');
                delete this._chunkBuffers[id];
                this.handleMessage(full);
            }
            return;
        }
        this.handleMessage(raw);
    } catch (error) {
        console.error('Fehler beim Verarbeiten der Nachricht:', error,
                      typeof event.data === 'string' ? event.data.slice(0, 200) : event.data);
    }
};

} catch (error) {
    console.error("Fehler beim Erstellen der WebSocket-Verbindung:", error);
    this.showMessage('Verbindungsfehler zum ESP32. Versuche erneut zu verbinden...', 'error');
    const delay = this.wsReconnectDelay || 5000;
    this.wsReconnectDelay = Math.min((this.wsReconnectDelay || 5000) * 2, 30000);
    setTimeout(() => this.connectWebSocket(), delay);
}
},  // ← Ende der connectWebSocket() Methode
                 
                 // Send message to server
                 sendMessage(data) {
                     if (this.socket && this.socket.readyState === WebSocket.OPEN) {
                         this.socket.send(JSON.stringify(data));
                     } else {
                         this.showMessage('Keine Verbindung zum Server', 'error');
                     }
                 },
                 
                 // Handle incoming messages
// Bytes formatierung
                 formatBytes(bytes) {
                     if (!bytes || bytes === 0) return '0 B';
                     const k = 1024;
                     const sizes = ['B', 'KB', 'MB', 'GB'];
                     const i = Math.floor(Math.log(bytes) / Math.log(k));
                     return parseFloat((bytes / Math.pow(k, i)).toFixed(1)) + ' ' + sizes[i];
                 },

                 // Speicherstatus anfordern
                 requestMemoryStatus() {
                     this.sendMessage({action: 'getMemoryStatus'});
                 },

                 handleMessage(message) {
                     try {
                         const data = JSON.parse(message);
                         console.log("Empfangene Nachricht:", data);
                         
// Handle memory status updates
if (data.type === "memoryStatus") {
    console.log("DEBUG: Speicherstatus empfangen:", data);
    this.memoryStatus = data;
    
    // Warnung bei kritischem Status
    if (data.status === 'critical') {
        console.warn('⚠️ Speicher kritisch niedrig!', data);
        this.showMessage('Speicher kritisch niedrig! System könnte instabil werden.', 'error');
    } else if (data.status === 'warning') {
        console.warn('⚠️ Speicher wird knapp!', data);
    }
    return;
}
                         
                         // Handle Schwankungskompensation
                         if (data.type === "dosingFactors") {
                             this.dosingFactors.enabled = data.enabled;
                             this.dosingFactors.hasNewPattern = data.hasNewPattern;
                             if (data.factors) {
                                 this.dosingFactors.factors = data.factors;
                             }
                             return;
                         }
                         if (data.type === "dosingFactorsStatus") {
                             this.dosingFactors.hasNewPattern = data.hasNewPattern;
                             return;
                         }

                         // Handle time updates
                         if (data.type === "timeUpdate") {
                             this.timeInitialized = data.timeInitialized;
                             this.currentTime = data.formattedTime || '--:--:--';
                             this.timeSource = data.timeSource || 'unbekannt';

                             // Wenn Zeit nicht initialisiert ist, Browser-Zeit senden
                             this.checkTimeSync();
                             return;
                         }
                         
                         // Handle pH voltage data
                         if (data.type === "phVoltage") {
                             this.updatePhVoltage(data);
                             return;
                         }
                         
                         // Handle pH calibration messages
                         if ((data.type === "info" && data.phCalibrationStatus === "running") ||
                             (data.type === "success" && data.phCalibrationStatus === "completed") ||
                             (data.type === "phCalibrationProgress")) {
                             this.processPhCalibrationMessage(data);
                             return;
                         }
                         
// Handle pH measurements
if (data.ph !== undefined) {
    this.updatePhTable(data);
    
    if (data.currentPH !== undefined) {
        this.phData.currentPH = data.currentPH;
    }
    
    if (data.rawVoltage !== undefined) {
        this.phData.rawVoltage = data.rawVoltage;
    }
    
    if (data.phCalibration !== undefined) {
        this.phData.phCalibration = data.phCalibration;
    }
    
    if (data.fixedTemperature !== undefined) {
        this.phData.fixedTemperature = data.fixedTemperature;
    }
    
    return;
}

// Handle consumption data
if (data.consumptionData) {
    this.consumptionData = data.consumptionData;
    this.$nextTick(() => {
        this.drawConsumptionChart();
    });
    return;
}

// Handle pH trend data
if (data.phTrendData) {
    console.log('pH-Trend-Daten empfangen:', data.phTrendData.length);
    this.phTrendData = data.phTrendData;
    this.$nextTick(() => {
        this.drawPhTrendChart();
    });
    return;
}
                         
                         // Handle pump status updates
                         if (data.pumps) {
                             this.updatePumpStatus(data);
                             
                             // Handle pump operation progress
                             if (data.activePumpIndex !== undefined && data.activePumpIndex >= 0) {
                                 this.updatePumpProgress(data);
                             } else {
                                 this.resetPumpProgress();
                             }
                         }
                         
// Handle system settings — inkrementelles Update um Toggle-Flicker zu vermeiden
if (data.settings) {
  const isAutoDosingLocked = this._autoDosingLockUntil && Date.now() < this._autoDosingLockUntil;
  Object.keys(data.settings).forEach(key => {
    // Während der Optimistic Lock aktiv ist, autoDosing nicht vom Server überschreiben
    // (verhindert Zurückspringen des Toggles durch verzögerte/alte Settings-Antworten)
    if (key === 'autoDosing' && isAutoDosingLocked) return;
    this.systemSettings[key] = data.settings[key];
  });
  // Anzeige auf 2 Nachkommastellen begrenzen
  if (this.systemSettings.initialKHConsumption != null) this.systemSettings.initialKHConsumption = parseFloat(this.systemSettings.initialKHConsumption.toFixed(2));
  if (this.systemSettings.initialCalciumConsumption != null) this.systemSettings.initialCalciumConsumption = parseFloat(this.systemSettings.initialCalciumConsumption.toFixed(2));
  if (this.systemSettings.antiDripSpeedML != null) this.systemSettings.antiDripSpeedML = parseFloat(this.systemSettings.antiDripSpeedML.toFixed(2));
}

// Handle WiFi configuration
if (data.ssid !== undefined) {
  this.wifiConfig.ssid = data.ssid;
  this.wifiConfig.configured = data.configured || false;
  this.wifiConfig.isAPMode = data.isAPMode || false;
  this.wifiConfig.ipAddress = data.ipAddress || '';
}

                         // Handle dosage plan
                         if (data.dosagePlan) {
                             this.updateDosagePlan(data);
                         }
                         
    // Handle KH dosage plan
    if (data.khDosagePlan) {
      this.khDosagePlan = data.khDosagePlan || [];
      
      if (data.navigation) {
        this.khDosagePlanDayOffset = data.navigation.currentDayOffset;
        this.maxKhDosageDays = data.navigation.daysAvailable;
        this.khDosagePlanDayTitle = data.navigation.dayName || `Tag ${this.khDosagePlanDayOffset + 1}`;
      }
    }
    
    // Handle Ca dosage plan
    if (data.caDosagePlan) {
      this.caDosagePlan = data.caDosagePlan || [];
      
      if (data.navigation) {
        this.caDosagePlanDayOffset = data.navigation.currentDayOffset;
        this.maxCaDosageDays = data.navigation.daysAvailable;
        this.caDosagePlanDayTitle = data.navigation.dayName || `Tag ${this.caDosagePlanDayOffset + 1}`;
      }
    }

// Handle dosage history
if (data.dosageHistory || data.navigation) {
    console.log("Empfange Dosierungshistorie:", data);
    this.updateDosageHistory(data);
}
                         
// Handle water measurements
if (data.kh !== undefined || data.calcium !== undefined || data.autoKh !== undefined) {
    this.updateWaterTables(data);
}

// Automatisch Dosierplan aktualisieren wenn updateDosage flag gesetzt ist
if (data.updateDosage) {
    this.getDosagePlan();
    this.getSystemSettings();
}

                         // Backup-Handler entfernt

                         // Handle info/error messages
                         if (data.type && data.message) {
                             this.showMessage(data.message, data.type);

                             // Bei erfolgreichen Dosierungs-Änderungen: Pläne aktualisieren
                             if (data.type === 'success' && data.updateDosage) {
                                 setTimeout(() => this.getDosagePlan(), 200);
                                 setTimeout(() => this.getDosageHistory(), 500);
                                 setTimeout(() => this.getSystemSettings(), 800);
                             }
                             // KEIN automatisches getStatus() mehr bei jedem success!
                             // Der Server sendet relevante Updates bereits direkt mit.
                         }
                     } catch (error) {
                         console.error('Fehler beim Verarbeiten der Nachricht:', error, message);
                     }
                 },
                 
// Show message
showMessage(text, type = 'info') {
    // Message-Box wurde entfernt, nur noch Console-Logging für Debug
    console.log(`[${type.toUpperCase()}] ${text}`);
    
    // Optional: Message im Data-Objekt speichern für interne Zwecke
    this.message = { text, type };
    
    // Auto-clear nach 5 Sekunden
    setTimeout(() => {
        if (this.message.text === text) {
            this.message = { text: '', type: 'info' };
        }
    }, 5000);
},

// Update pump status
updatePumpStatus(data) {
  this.pumps = data.pumps;
  this.currentCalibrationPump = data.currentCalibrationPump;
  this.currentCalibrationSteps = data.currentCalibrationSteps;
  
  // Update calibration form status
  this.calibration.running = this.currentCalibrationPump >= 0;
  
  // Update last dispense information
  if (data.lastPumpOperation >= 0) {
    this.lastPumpOperation = data.lastPumpOperation;
    this.lastPumpAmount = data.lastPumpAmount;
    this.lastDispenseMessage = `Letzte Dosierung: ${data.lastPumpAmount} ml von ${this.getPumpName(data.lastPumpOperation)}`;
  }
  
  // Update active pump information
  this.activePumpIndex = data.activePumpIndex !== undefined ? data.activePumpIndex : -1;
  this.targetSteps = data.targetSteps !== undefined ? data.targetSteps : 0;
  this.isCalibrationRunning = data.isCalibrationRunning === true;
  this.isDispensingRunning = data.isDispensingRunning === true;
  
  // NEUE ZEILEN: Update global settings from first pump
  if (data.pumps && data.pumps.length > 0) {
    this.globalSettings.speedML = parseFloat((data.pumps[0].speedML || 3.6).toFixed(2));
    this.globalSettings.accelerationML = parseFloat((data.pumps[0].accelerationML || 1.8).toFixed(2));
  }
},
                 
                 // Update pump progress indicators
                 updatePumpProgress(data) {
                     const activePump = data.activePumpIndex;
                     const total = data.targetSteps;
                     
                     // Simulate progress
                     const percentComplete = Math.min(100, Math.round((Math.random() * 0.3 + 0.6) * 100));
                     const currentSteps = Math.floor(total * percentComplete / 100);
                     
                     if (this.isCalibrationRunning) {
                         this.calibrationProgress = {
                             show: true,
                             percent: percentComplete,
                             currentSteps: currentSteps,
                             totalSteps: total
                         };
                     } else if (this.isDispensingRunning) {
                         this.dispensingProgress = {
                             show: true,
                             percent: percentComplete,
                             currentSteps: currentSteps,
                             totalSteps: total
                         };
                     }
                 },
                 
                 // Reset pump progress indicators
                 resetPumpProgress() {
                     this.calibrationProgress.show = false;
                     this.dispensingProgress.show = false;
                 },
                 
                 // Get a pump name by its index
                 getPumpName(index) {
                     const pump = this.pumps.find(p => p.id === index);
                     return pump ? pump.name : `Pumpe ${index}`;
                 },
                 
                 // Update system settings
                 updateSystemSettings(settings) {
                      this.systemSettings = settings;
                 },
                 
                 // Get status from server
                 getStatus() {
                     this.sendMessage({action: 'getStatus'});
                 },
                 
                 // Get system settings from server
                 getSystemSettings() {
                     this.sendMessage({action: 'getSystemSettings'});
                 },
                 
                 // Get dosage plan from server
                 getDosagePlan(dayOffset = 0) {
                     this.sendMessage({
                         action: 'getDosagePlan',
                         dayOffset: dayOffset
                     });
                 },

// Neue Methoden für getrennte Dosierpläne
getKhDosagePlan() {
  this.sendMessage({
    action: 'getAllKhDosagePlan'
  });
},

getCaDosagePlan() {
  this.sendMessage({
    action: 'getAllCaDosagePlan'
  });
},
                 
                 // Update dosage plan data
                 updateDosagePlan(data) {
                     this.dosagePlan = data.dosagePlan || [];
                     
                     if (data.navigation) {
                         this.dosagePlanDayOffset = data.navigation.currentDayOffset;
                         this.maxDosageDays = data.navigation.daysAvailable;
                         this.dosagePlanDayTitle = data.navigation.dayName || `Tag ${this.dosagePlanDayOffset + 1}`;
                     }
                 },
                 
                 // Navigate dosage plan days
                 navigateDosagePlan(direction) {
                     const newOffset = this.dosagePlanDayOffset + direction;
                     if (newOffset >= 0 && newOffset < this.maxDosageDays) {
                         this.getDosagePlan(newOffset);
                     }
                 },
                 
                 // Get dosage history from server
getDosageHistory() {
  this.sendMessage({
    action: 'getDosageHistory',
    dayOffset: this.currentHistoryDayOffset
  });
},
                 
// Verarbeitung der Serverantwort
updateDosageHistory(data) {
  // Sicherheitsabfragen
  if (!data) {
    console.error("Leere Serverantwort erhalten");
    return;
  }
  
  // Dosierungsdaten extrahieren
  if (data.dosageHistory) {
    this.dosageHistory = data.dosageHistory;
    console.log(`${this.dosageHistory.length} Dosierungen geladen`);
  } else if (Array.isArray(data)) {
    // Fallback für reine Arrays (für Kompatibilität)
    this.dosageHistory = data;
    console.log(`${this.dosageHistory.length} Dosierungen geladen (Array-Format)`);
  } else {
    console.error("Unerwartetes Datenformat:", data);
    this.dosageHistory = [];
  }
  
// Navigationsdaten verarbeiten
if (data.navigation) {
  const nav = data.navigation;
  
  // Tage-Liste aktualisieren, wenn sie vom Server gesendet wird
  if (nav.days && Array.isArray(nav.days)) {
    this.historyDays = nav.days;
    console.log(`${this.historyDays.length} verfügbare Tage erhalten:`, this.historyDays);
    
    // Sicherstellen, dass currentHistoryDayOffset gültig ist
    if (this.currentHistoryDayOffset >= this.historyDays.length) {
      this.currentHistoryDayOffset = 0;
    }
  }
    
    // Tag-Titel aktualisieren
    if (nav.currentDay) {
      this.dosageHistoryDayTitle = nav.currentDay;
      console.log(`Aktueller Tag: ${this.dosageHistoryDayTitle}`);
    }
    
    // Offset aktualisieren
    if (nav.currentDayOffset !== undefined) {
      this.currentHistoryDayOffset = nav.currentDayOffset;
      console.log(`Aktueller Offset: ${this.currentHistoryDayOffset}`);
    }
  } else {
    console.warn("Keine Navigationsdaten vom Server erhalten");
  }
},

// Diese computed property wurde bereits oben definiert, daher entfernt
                 
navigateHistoryDay(direction) {
  // Sicherheitscheck
  if (!this.historyDays || this.historyDays.length === 0) {
    console.log("Keine Tage verfügbar für Navigation");
    return;
  }
  
  console.log("Verfügbare Tage:", this.historyDays);
  
  // Berechne neuen Offset
  const newOffset = this.currentHistoryDayOffset + direction;
  
  console.log(`Navigation: ${this.currentHistoryDayOffset} -> ${newOffset}, verfügbare Tage: ${this.historyDays.length}`);
  console.log(`Aktueller Tag: ${this.historyDays[this.currentHistoryDayOffset]}, Neuer Tag: ${this.historyDays[newOffset]}`);


  // Prüfe ob der neue Offset gültig ist
  if (newOffset >= 0 && newOffset < this.historyDays.length) {
    // Aktualisiere lokalen Offset
    this.currentHistoryDayOffset = newOffset;
    
    // Aktualisiere den Titel sofort für bessere Benutzererfahrung
    this.dosageHistoryDayTitle = this.historyDays[this.currentHistoryDayOffset];
    
    // Anfrage an Server senden
    this.sendMessage({
      action: 'getDosageHistory',
      dayOffset: this.currentHistoryDayOffset
    });
  } else {
    console.log(`Navigation ungültig: Offset ${newOffset} außerhalb des gültigen Bereichs (0-${this.historyDays.length-1})`);
  }
},
                 
                 // Get water measurements from server
                 getWaterMeasurements() {
                     this.sendMessage({
                         action: 'getWaterMeasurements',
                         weeks: this.currentWeeks
                     });
                 },
                 
                 // Get all water measurements for tables
                 getAllWaterMeasurements() {
                     this.sendMessage({action: 'getAllWaterMeasurements'});
                 },
                                                                    
                 // Save new water measurement
                 saveWaterMeasurement() {
                     const kh = parseFloat(this.newMeasurement.kh);
                     const calcium = parseFloat(this.newMeasurement.calcium);
                     
                     if (isNaN(kh) && isNaN(calcium)) {
                         this.showMessage('Bitte mindestens einen Wert eingeben', 'error');
                         return;
                     }
                     
                     this.sendMessage({
                         action: 'saveWaterMeasurement',
                         kh: isNaN(kh) ? 0 : kh,
                         calcium: isNaN(calcium) ? 0 : calcium
                     });
                     
                     // Clear input fields
                     this.newMeasurement.kh = '';
                     this.newMeasurement.calcium = '';
                     
// Update data after saving
setTimeout(() => {
    this.getAllWaterMeasurements();
    this.getDosagePlan();
    this.getSystemSettings();
}, 500);
},
                 
// Delete water measurement
deleteWaterMeasurement(index, isCalcium) {
    if (confirm('Möchten Sie diesen Messwert wirklich löschen?')) {
        this.sendMessage({
            action: 'deleteWaterData',
            index: index,
            isCalcium: isCalcium
        });

        // Keine automatische Aktualisierung hier - warte auf Server-Antwort
    }
},

// Auto-KH-Wert in reguläre KH-Messungen übernehmen
adoptAutoKHMeasurement(index, value) {
    if (confirm('KH-Wert ' + value.toFixed(1) + ' °dKH in Messungen übernehmen und Dosierplan neu berechnen?')) {
        this.sendMessage({
            action: 'adoptAutoKH',
            index: index
        });
    }
},

// NEU: Delete automatic KH measurement
deleteAutoKHMeasurement(index) {
    if (confirm('Möchten Sie diesen automatischen KH-Messwert wirklich löschen?')) {
        this.sendMessage({
            action: 'deleteWaterData',
            index: index,
            isAutoKh: true
        });

        // Keine automatische Aktualisierung hier - warte auf Server-Antwort
    }
},

// Schwankungskompensation: Faktoren berechnen
calculateDosingFactors() {
    this.sendMessage({ action: 'calculateDosingFactors' });
},

// Schwankungskompensation: Ein/Aus
toggleDosingFactors() {
    this.sendMessage({ action: 'toggleDosingFactors', enabled: this.dosingFactors.enabled });
},

// Schwankungskompensation: Faktoren zurücksetzen
resetDosingFactors() {
    if (confirm('Alle Faktoren und gespeicherte Muster zurücksetzen?')) {
        this.sendMessage({ action: 'resetDosingFactors' });
    }
},

// Delete dosage
deleteDosage(index, dosageType) {
    if (confirm('Möchten Sie diese Dosierung wirklich löschen?')) {
        this.sendMessage({
            action: 'deleteWaterData',
            index: index,
            dosageType: dosageType
        });
        
        // Keine automatische Aktualisierung hier - warte auf Server-Antwort
    }
},
                 
                 // Toggle collapsible sections
                 toggleCollapsible(section) {
                     this.collapsible[section] = !this.collapsible[section];
                 },
                 
                 // Toggle automatic dosing
                 toggleAutoDosing() {
                     // Optimistic Lock: eingehende Settings-Updates für autoDosing
                     // 3 Sekunden lang ignorieren, damit der Toggle nicht zurückspringt
                     this._autoDosingLockUntil = Date.now() + 3000;
                     this.sendMessage({
                         action: 'setAutoDosing',
                         autoDosing: this.systemSettings.autoDosing
                     });
                 },
                 
                 // Save system settings
                 saveSettings() {
                     this.sendMessage({
                         action: 'saveSystemSettings',
                         settings: {
                             aquariumVolume: this.systemSettings.aquariumVolume,
                             targetKH: this.systemSettings.targetKH,
                             targetCalcium: this.systemSettings.targetCalcium,
                             historyCount: this.systemSettings.historyCount,
                             maxDailyChangeKH: this.systemSettings.maxDailyChangeKH,
                             maxDailyChangeCalcium: this.systemSettings.maxDailyChangeCalcium,
                             magnesiumRatio: this.systemSettings.magnesiumRatio,
                             khNightStart: this.systemSettings.khNightStart,
                             khNightEnd: this.systemSettings.khNightEnd,
                             initialKHConsumption: this.systemSettings.initialKHConsumption,
                             initialCalciumConsumption: this.systemSettings.initialCalciumConsumption,
                             autoUpdateInitialRates: this.systemSettings.autoUpdateInitialRates,
                             containerCapacity0: this.systemSettings.containers[0]?.capacity,
                             containerCapacity1: this.systemSettings.containers[1]?.capacity,
                             containerCapacity2: this.systemSettings.containers[2]?.capacity,
                             containerCapacity3: this.systemSettings.containers[3]?.capacity,
                             usePhBasedKHDosing: this.systemSettings.usePhBasedKHDosing,
                             phThresholdForKHNight: this.systemSettings.phThresholdForKHNight,
                             enableAntiDrip: this.systemSettings.enableAntiDrip,
                             antiDripML: this.systemSettings.antiDripML,
                             antiDripSpeedML: this.systemSettings.antiDripSpeedML,
                             timeOffset: this.systemSettings.timeOffset
                         }
                     });
                 },
                 
// WiFi-Konfiguration speichern
                 saveWiFiConfig() {
                     if (!this.wifiSettings.ssid) {
                         alert('Bitte gib einen WLAN-Namen (SSID) ein!');
                         return;
                     }

                     if (confirm('WLAN-Einstellungen speichern und ESP32 neu starten?\n\nSSID: ' + this.wifiSettings.ssid)) {
                         this.sendMessage({
                             action: 'saveWiFiConfig',
                             ssid: this.wifiSettings.ssid,
                             password: this.wifiSettings.password
                         });
                     }
                 },

                 // WiFi-Konfiguration laden
                 loadWiFiConfig() {
                     this.sendMessage({action: 'getWiFiConfig'});
                 },

                 // Browser-Zeit an ESP32 senden (für AP-Modus ohne Internet)
                 sendBrowserTime() {
                     // Aktuelle Browser-Zeit in Unix-Timestamp (Sekunden)
                     const browserTimestamp = Math.floor(Date.now() / 1000);

                     console.log('Sende Browser-Zeit an ESP32:', new Date(browserTimestamp * 1000));

                     this.sendMessage({
                         action: 'setBrowserTime',
                         timestamp: browserTimestamp
                     });
                 },

                 // Prüfen ob Zeit synchronisiert werden muss (automatisch)
                 checkTimeSync() {
                     if (!this.timeInitialized && this.connected) {
                         console.log('Zeit nicht initialisiert - sende Browser-Zeit automatisch');
                         this.sendBrowserTime();
                     }
                 },

                 // Zeit-Status Tooltip
                 getTimeStatusTitle() {
                     if (this.timeInitialized) {
                         return 'Zeit synchronisiert (Quelle: ' + (this.timeSource || 'unbekannt') + ')';
                     } else {
                         return 'Zeit wird automatisch synchronisiert...';
                     }
                 },

// Reset Settings
                 resetSettings() {
                     if (confirm('Möchten Sie wirklich alle Einstellungen zurücksetzen? Alle Kalibrierungsdaten, Messungen und Einstellungen werden gelöscht!')) {
                         this.sendMessage({action: 'resetSettings'});
                     }
                 },


                 // Start calibration
                 startCalibration() {
                     const pumpIndex = parseInt(this.calibration.pumpIndex);
                     const steps = parseInt(this.calibration.steps);
                     
                     if (steps <= 0) {
                         this.showMessage('Schritte müssen größer als 0 sein', 'error');
                         return;
                     }
                     
                     this.sendMessage({
                         action: 'calibrate',
                         pump: pumpIndex,
                         steps: steps
                     });
                     
                     this.calibrationProgress = {
                         show: true,
                         percent: 0,
                         currentSteps: 0,
                         totalSteps: steps
                     };
                 },
                 
                 // Save calibration
                 saveCalibration() {
                     const ml = parseFloat(this.calibration.ml);
                     
                     if (isNaN(ml) || ml <= 0) {
                         this.showMessage('Bitte geben Sie eine gültige Menge ein', 'error');
                         return;
                     }
                     
                     this.sendMessage({
                         action: 'saveCalibration',
                         pump: this.currentCalibrationPump,
                         ml: ml
                     });
                     
                     // Reset calibration form
                     this.calibration = {
                         pumpIndex: 0,
                         steps: 1000,
                         ml: 0,
                         running: false
                     };
                 },
                 
                 // Start dispensing
                 startDispense() {
                     const pumpIndex = parseInt(this.dispense.pumpIndex);
                     const ml = parseFloat(this.dispense.ml);
                     
                     if (isNaN(ml) || ml <= 0) {
                         this.showMessage('Bitte geben Sie eine gültige Menge ein', 'error');
                         return;
                     }
                     
                     this.sendMessage({
                         action: 'dispense',
                         pump: pumpIndex,
                         ml: ml,
                         isAutomatic: false
                     });
                     
                     // Find the selected pump to get steps per ml for progress
                     const selectedPump = this.pumps.find(p => p.id === pumpIndex);
                     if (selectedPump && selectedPump.mlPerStep) {
                         const estimatedSteps = Math.round(ml / selectedPump.mlPerStep);
                         this.dispensingProgress = {
                             show: true,
                             percent: 0,
                             currentSteps: 0,
                             totalSteps: estimatedSteps
                         };
                     }
                 },
                 
                 // Set global pump settings
                 setGlobalSettings() {
                     const speedML = parseFloat(this.globalSettings.speedML);
                     const accelerationML = parseFloat(this.globalSettings.accelerationML);

                     if (speedML < 0) {
                         this.showMessage('Geschwindigkeit muss größer oder gleich 0 sein', 'error');
                         return;
                     }

                     if (accelerationML < 0) {
                         this.showMessage('Beschleunigung muss größer oder gleich 0 sein', 'error');
                         return;
                     }

                     this.sendMessage({
                         action: 'setGlobalSettings',
                         speedML: speedML,
                         accelerationML: accelerationML
                     });
                 },
                 
                 // Refill container
                 refillContainer(containerId) {
                     this.sendMessage({
                         action: 'refillContainer',
                         container: containerId
                     });
                 },

                     // Neue Methode zur automatischen Speicherung der Kanisterkapazität
    saveContainerCapacity(containerId, capacity) {
        // Validiere Eingabe
        if (capacity <= 0 || capacity > 50000) {
            this.showMessage('Bitte geben Sie einen gültigen Wert zwischen 1 und 50000 ein', 'error');
            return;
        }
        
        // Sofort an ESP senden
        this.sendMessage({
            action: 'updateContainerCapacity',
            container: containerId,
            capacity: capacity
        });
        
        // Feedback für den Benutzer
        this.showMessage('Kapazität für ' + this.systemSettings.containers[containerId].name + 
                         ' wurde auf ' + capacity + ' ml aktualisiert', 'info');
    },
                 
                 // Format days remaining for container display
                 formatDaysRemaining(days) {
                     if (days == null) return '—';
                     if (days >= 999) {
                         return 'Unbegrenzt';
                     }
                     
                     const daysText = days.toFixed(1) + ' Tage';
                     
                     if (days < 3) {
                         return `<span style="color: var(--danger-color); font-weight: bold;">${daysText}</span>`;
                     } else if (days < 7) {
                         return `<span style="color: var(--warning-color); font-weight: bold;">${daysText}</span>`;
                     }
                     
                     return daysText;
                 },
                 
                 // Get color for container progress bar
                 getContainerColor(percentage) {
                     if (percentage < 30) return '#f44336';  // Red
                     if (percentage < 70) return '#ff9800';  // Orange
                     return '#4CAF50';  // Green
                 },
                 
// Get CSS class for value highlighting
getValueHighlightClass(value, targetValue, threshold, warningThreshold) {
    if (value === undefined || value === null || targetValue === undefined || targetValue === null) {
        return '';
    }
    
    const diff = Math.abs(value - targetValue);
    
    if (diff <= threshold) {
        return 'highlight-good';
    } else if (diff <= warningThreshold) {
        return 'highlight-warning';
    } else {
        return 'highlight-bad';
    }
},

// Get CSS class for pH value highlighting
getPhValueHighlightClass(value) {
    if (value === undefined || value === null) return '';
    
    if (value < 6.0) return 'highlight-bad';
    if (value > 8.0) return 'highlight-warning';
    return 'highlight-good';
},


// Neue Methode zur Bestimmung, ob die aktuelle Zeit im Nachtbereich liegt
isCurrentTimeInNightRange() {
    if (!this.currentTime) return false;
    
    const hourMatch = this.currentTime.match(/(\d+):/);
    if (!hourMatch) return false;
    
    const currentHour = parseInt(hourMatch[1], 10);
    
    if (this.systemSettings.khNightStart < this.systemSettings.khNightEnd) {
        // Normaler Fall (z.B. 19-7 Uhr)
        return currentHour >= this.systemSettings.khNightStart && currentHour < this.systemSettings.khNightEnd;
    } else {
        // Über Mitternacht (z.B. 22-6 Uhr)
        return currentHour >= this.systemSettings.khNightStart || currentHour < this.systemSettings.khNightEnd;
    }
},

                 // pH-related methods
                 getPhMeasurements() {
                     this.sendMessage({action: 'getPhMeasurements'});
                 },
                 
                 measurePh() {
                     this.sendMessage({action: 'measurePh'});
                 },
                 
                 calibratePh(phValue) {
                     this.sendMessage({
                         action: 'calibratePh',
                         phValue: phValue
                     });
                 },
                 
deletePhMeasurement(index) {
    if (confirm('Möchten Sie diesen pH-Messwert wirklich löschen?')) {
        this.sendMessage({
            action: 'deletePhMeasurement',
            index: index
        });
        
        // Keine automatische Aktualisierung hier - warte auf Server-Antwort
    }
},

                 updateWaterTables(data) {
    // Aktualisiert sowohl KH als auch Calcium Tabellen basierend auf den empfangenen Daten
    if (data.kh) {
        this.waterMeasurements.kh = data.kh.map(item => ({
            ...item,
            date: item.date || this.formatDateTime(item.timestamp)
        }));
    }

    if (data.calcium) {
        this.waterMeasurements.calcium = data.calcium.map(item => ({
            ...item,
            date: item.date || this.formatDateTime(item.timestamp)
        }));
    }

    // NEU: Automatische KH-Messungen
    if (data.autoKh) {
        this.waterMeasurements.autoKh = data.autoKh.map(item => ({
            ...item,
            date: item.date || this.formatDateTime(item.timestamp)
        }));
    }

    // Log für Debugging
    console.log("Wassermessungstabellen aktualisiert:",
                "KH:", this.waterMeasurements.kh.length,
                "Ca:", this.waterMeasurements.calcium.length,
                "Auto-KH:", this.waterMeasurements.autoKh.length);
},
                 
                 updatePhTable(data) {
                     if (!data.ph) return;
                     
                     this.phData.ph = data.ph;
                     this.phData.currentPH = data.currentPH;
                     this.phData.isCalibrating = data.isCalibrating;
                     this.phData.isCalibrationStable = data.isCalibrationStable;
                     this.phData.fixedTemperature = data.fixedTemperature;
                     
                     // Extract unique days from history
                     this.phHistoryDays = [];
                     const dayMap = {};
                     
                     // Sort by date (newest first)
                     this.phData.ph.sort((a, b) => b.timestamp - a.timestamp);
                     
                     // Extract unique days
                     this.phData.ph.forEach(measurement => {
                         const date = measurement.date.split(' ')[0];
                         if (!dayMap[date]) {
                             dayMap[date] = true;
                             this.phHistoryDays.push(date);
                         }
                     });
                     
                     // Sort days (newest first)
                     this.phHistoryDays.sort((a, b) => {
                         const partsA = a.split('.');
                         const partsB = b.split('.');
                         const dateA = new Date(partsA[2], partsA[1]-1, partsA[0]);
                         const dateB = new Date(partsB[2], partsB[1]-1, partsB[0]);
                         return dateB - dateA;
                     });
                     
                     // Update day title
                     if (this.currentPhHistoryDayOffset === -1) {
                         this.phHistoryDayTitle = 'Alle Messungen';
                     } else if (this.phHistoryDays.length > 0 && 
                               this.currentPhHistoryDayOffset >= 0 && 
                               this.currentPhHistoryDayOffset < this.phHistoryDays.length) {
                         this.phHistoryDayTitle = this.phHistoryDays[this.currentPhHistoryDayOffset];
                     } else {
                         this.phHistoryDayTitle = 'Keine Messungen';
                     }
                 },
                                  
                 navigatePhHistory(direction) {
                     if (direction < 0 && this.currentPhHistoryDayOffset === -1) {
                         return; // Already at "All Measurements"
                     }
                     
                     if (direction > 0 && this.currentPhHistoryDayOffset === this.phHistoryDays.length - 1) {
                         this.currentPhHistoryDayOffset = -1; // Go to "All Measurements"
                         this.phHistoryDayTitle = 'Alle Messungen';
                         return;
                     }
                     
                     const newOffset = this.currentPhHistoryDayOffset + direction;
                     if (newOffset >= -1 && newOffset < this.phHistoryDays.length) {
                         this.currentPhHistoryDayOffset = newOffset;
                         
                         if (newOffset === -1) {
                             this.phHistoryDayTitle = 'Alle Messungen';
                         } else {
                             this.phHistoryDayTitle = this.phHistoryDays[newOffset];
                         }
                     }
                 },
                 
                 processPhCalibrationMessage(data) {
                     if (data.type === "phCalibrationProgress") {
                         this.phCalibrationProgress = data.progress;
                     }
                     
                     if (data.type === "info" && data.phCalibrationStatus === "running") {
                         this.phData.isCalibrating = true;
                         this.phData.isCalibrationStable = false;
                     }
                     
                     if (data.type === "success" && data.phCalibrationStatus === "completed") {
                         this.phData.isCalibrationStable = true;
                         
                         setTimeout(() => {
                             this.phData.isCalibrating = false;
                             this.phData.isCalibrationStable = false;
                         }, 5000);
                     }
                 },
                 
                 updatePhVoltage(data) {
                     this.phData.rawVoltage = data.rawVoltage;
                     if (data.currentPH !== undefined) {
                         this.phData.currentPH = data.currentPH;
                     }
                 },
                 
                 // Handle pH voltage monitoring
                 startVoltageMonitoring() {
                     this.voltageMonitoringInterval = setInterval(() => {
                         this.requestLiveVoltage();
                     }, 500);
                 },
                 
                 stopVoltageMonitoring() {
                     clearInterval(this.voltageMonitoringInterval);
                     this.voltageMonitoringInterval = null;
                 },
                 
requestLiveVoltage() {
                     this.sendMessage({action: "getPhVoltageLive"});
                 },
                 
                 // New consumption chart methods
loadConsumptionData() {
                     this.sendMessage({action: 'getConsumptionData'});
                 },
                 
                 // Load pH trend data
                 loadPhTrendData() {
                     this.sendMessage({action: 'getPhTrendData'});
                 },
                 
                 getChartTitle() {
                     if (!this.consumptionData.length) {
                         return 'Verbrauchsanalyse';
                     }
                     
                     const days = this.consumptionData.length;
                     const hasEstimates = this.consumptionData.some(d => d.isEstimate);
                     
                     if (days === 1) {
                         return 'Verbrauchsanalyse (1 Tag)';
                     } else if (days <= 7) {
                         return `Verbrauchsanalyse (${days} Tage)`;
                     } else if (days <= 30) {
                         const weeks = Math.round(days / 7);
                         return `Verbrauchsanalyse (${weeks} ${weeks === 1 ? 'Woche' : 'Wochen'})`;
                     } else {
                         const months = Math.round(days / 30);
                         return `Verbrauchsanalyse (${months} ${months === 1 ? 'Monat' : 'Monate'})`;
                     }
                 },
                 
drawConsumptionChart() {
    if (!this.consumptionData.length) return;
    
    const svg = document.querySelector('#chartContent');
    const chartWidth = 720;
    const chartHeight = 240;
    
    // Clear existing content
    svg.innerHTML = '';
    
    // Find data ranges
    const khValues = this.consumptionData.map(d => d.khConsumption).filter(v => v > 0);
    const caValues = this.consumptionData.map(d => d.caConsumption).filter(v => v > 0);
    
    if (!khValues.length && !caValues.length) {
        svg.innerHTML = '<text x="360" y="120" text-anchor="middle" fill="#999">Keine Verbrauchsdaten verfügbar</text>';
        return;
    }
    
    const maxKH = Math.max(...khValues, 1);
    const maxCa = Math.max(...caValues, 1);
    const dataLength = this.consumptionData.length;
    
    // Create scales
    const xScale = (index) => (index / Math.max(dataLength - 1, 1)) * chartWidth;
    const yScaleKH = (value) => chartHeight - (value / maxKH) * chartHeight;
    const yScaleCa = (value) => chartHeight - (value / maxCa) * chartHeight;
    
    // Helper function for linear regression
    const calculateTrendLine = (values) => {
        if (values.length < 2) return null;
        
        let sumX = 0, sumY = 0, sumXY = 0, sumXX = 0;
        const n = values.length;
        
        values.forEach((value, index) => {
            sumX += index;
            sumY += value;
            sumXY += index * value;
            sumXX += index * index;
        });
        
        const slope = (n * sumXY - sumX * sumY) / (n * sumXX - sumX * sumX);
        const intercept = (sumY - slope * sumX) / n;
        
        return { slope, intercept };
    };
    
    // Calculate trend lines
    const khTrend = calculateTrendLine(khValues);
    const caTrend = calculateTrendLine(caValues);
    
    // Draw trend lines first (behind data)
    if (khTrend && khValues.length >= 3) {
        const startY = yScaleKH(khTrend.intercept);
        const endY = yScaleKH(khTrend.intercept + khTrend.slope * (dataLength - 1));
        
        const trendLine = document.createElementNS('http://www.w3.org/2000/svg', 'line');
        trendLine.setAttribute('x1', '0');
        trendLine.setAttribute('y1', startY);
        trendLine.setAttribute('x2', chartWidth);
        trendLine.setAttribute('y2', endY);
        trendLine.setAttribute('stroke', '#2196F3');
        trendLine.setAttribute('stroke-width', '1');
        trendLine.setAttribute('stroke-dasharray', '5,5');
        trendLine.setAttribute('opacity', '0.7');
        
        const trendDirection = khTrend.slope > 0 ? '↗' : khTrend.slope < 0 ? '↘' : '→';
        trendLine.innerHTML = `<title>KH-Trend: ${trendDirection} ${Math.abs(khTrend.slope).toFixed(3)} dKH/Tag pro Tag</title>`;
        svg.appendChild(trendLine);
    }
    
    if (caTrend && caValues.length >= 3) {
        const startY = yScaleCa(caTrend.intercept);
        const endY = yScaleCa(caTrend.intercept + caTrend.slope * (dataLength - 1));
        
        const trendLine = document.createElementNS('http://www.w3.org/2000/svg', 'line');
        trendLine.setAttribute('x1', '0');
        trendLine.setAttribute('y1', startY);
        trendLine.setAttribute('x2', chartWidth);
        trendLine.setAttribute('y2', endY);
        trendLine.setAttribute('stroke', '#ff9800');
        trendLine.setAttribute('stroke-width', '1');
        trendLine.setAttribute('stroke-dasharray', '5,5');
        trendLine.setAttribute('opacity', '0.7');
        
        const trendDirection = caTrend.slope > 0 ? '↗' : caTrend.slope < 0 ? '↘' : '→';
        trendLine.innerHTML = `<title>Ca-Trend: ${trendDirection} ${Math.abs(caTrend.slope).toFixed(3)} mg/l/Tag pro Tag</title>`;
        svg.appendChild(trendLine);
    }
    
    // Draw KH line
    let khPath = '';
    let caPath = '';
    
    this.consumptionData.forEach((data, index) => {
        const x = xScale(index);
        
        if (data.khConsumption > 0) {
            const y = yScaleKH(data.khConsumption);
            khPath += (khPath ? ' L ' : 'M ') + x + ' ' + y;
        }
        
        if (data.caConsumption > 0) {
            const y = yScaleCa(data.caConsumption);
            caPath += (caPath ? ' L ' : 'M ') + x + ' ' + y;
        }
    });
    
    // Add KH line
    if (khPath) {
        const khLine = document.createElementNS('http://www.w3.org/2000/svg', 'path');
        khLine.setAttribute('d', khPath);
        khLine.setAttribute('stroke', '#2196F3');
        khLine.setAttribute('stroke-width', '2');
        khLine.setAttribute('fill', 'none');
        svg.appendChild(khLine);
    }
    
    // Add Ca line
    if (caPath) {
        const caLine = document.createElementNS('http://www.w3.org/2000/svg', 'path');
        caLine.setAttribute('d', caPath);
        caLine.setAttribute('stroke', '#ff9800');
        caLine.setAttribute('stroke-width', '2');
        caLine.setAttribute('fill', 'none');
        svg.appendChild(caLine);
    }
    
    // Add data points
    this.consumptionData.forEach((data, index) => {
        const x = xScale(index);
        
        if (data.khConsumption > 0) {
            const y = yScaleKH(data.khConsumption);
            const circle = document.createElementNS('http://www.w3.org/2000/svg', 'circle');
            circle.setAttribute('cx', x);
            circle.setAttribute('cy', y);
            circle.setAttribute('r', '3');
            circle.setAttribute('fill', '#2196F3');
            circle.innerHTML = `<title>KH: ${data.khConsumption.toFixed(2)} dKH/Tag am ${data.date}</title>`;
            svg.appendChild(circle);
        }
        
        if (data.caConsumption > 0) {
            const y = yScaleCa(data.caConsumption);
            const circle = document.createElementNS('http://www.w3.org/2000/svg', 'circle');
            circle.setAttribute('cx', x);
            circle.setAttribute('cy', y);
            circle.setAttribute('r', '3');
            circle.setAttribute('fill', '#ff9800');
            circle.innerHTML = `<title>Ca: ${data.caConsumption.toFixed(2)} mg/l/Tag am ${data.date}</title>`;
            svg.appendChild(circle);
        }
    });
    
    // Add Y-axis scale labels (mit mehr Platz)
    const yAxisKH = document.querySelector('#yAxisKH');
    const yAxisCa = document.querySelector('#yAxisCa');
    
    // Clear existing labels
    yAxisKH.querySelectorAll('.scale-label').forEach(el => el.remove());
    yAxisCa.querySelectorAll('.scale-label').forEach(el => el.remove());
    
// Add KH scale labels (linke Seite)
for (let i = 0; i <= 4; i++) {
    const value = (maxKH / 4) * i;
    const y = 20 + chartHeight - (i / 4) * chartHeight;
    const text = document.createElementNS('http://www.w3.org/2000/svg', 'text');
    text.setAttribute('x', '-25');
    text.setAttribute('y', y + 4);
    text.setAttribute('font-size', '10');
    text.setAttribute('fill', '#2196F3');
    text.setAttribute('text-anchor', 'end');
    text.setAttribute('class', 'scale-label');
    text.setAttribute('font-weight', 'bold');
    text.textContent = value.toFixed(1);
    yAxisKH.appendChild(text);
}

// Add Ca scale labels (rechte Seite)
for (let i = 0; i <= 4; i++) {
    const value = (maxCa / 4) * i;
    const y = 20 + chartHeight - (i / 4) * chartHeight;
    const text = document.createElementNS('http://www.w3.org/2000/svg', 'text');
    text.setAttribute('x', '25');
    text.setAttribute('y', y + 4);
    text.setAttribute('font-size', '10');
    text.setAttribute('fill', '#ff9800');
    text.setAttribute('text-anchor', 'start');
    text.setAttribute('class', 'scale-label');
    text.setAttribute('font-weight', 'bold');
    text.textContent = value.toFixed(1);
    yAxisCa.appendChild(text);
}
    
    // Add X-axis dates (intelligente Label-Verteilung, aber alle Datenpunkte)
    const xAxis = document.querySelector('#xAxis');
    xAxis.querySelectorAll('.date-label').forEach(el => el.remove());
    
    // Berechne optimale Anzahl von Labels (max 8-10 Labels für Lesbarkeit)
    const maxLabels = 8;
    let labelInterval = Math.max(1, Math.floor(dataLength / maxLabels));
    
    // Beginne mit dem ersten und letzten Datum, plus gleichmäßig verteilte Labels
    const labelIndices = new Set();
    
    // Immer erstes und letztes Datum zeigen
    labelIndices.add(0);
    if (dataLength > 1) {
        labelIndices.add(dataLength - 1);
    }
    
    // Gleichmäßig verteilte Labels dazwischen
    for (let i = labelInterval; i < dataLength - 1; i += labelInterval) {
        labelIndices.add(i);
    }
    
    // Labels erstellen
    Array.from(labelIndices).sort((a, b) => a - b).forEach(i => {
        if (this.consumptionData[i]) {
            const x = xScale(i);
const text = document.createElementNS('http://www.w3.org/2000/svg', 'text');
text.setAttribute('x', x);
text.setAttribute('y', '15');
text.setAttribute('font-size', '10');
text.setAttribute('fill', '#666');
text.setAttribute('text-anchor', 'middle');
text.setAttribute('class', 'date-label');
text.setAttribute('font-weight', 'bold');
text.textContent = this.consumptionData[i].date.split(' ')[0];
xAxis.appendChild(text);
        }
});
},

// Neue Datenverarbeitung für SVG - KORRIGIERT
processPhDataForSvg(phTrendData) {
    if (!phTrendData?.length) return { dayGroups: {}, recent12: [] };
    
    // Gruppiere nach Tagen
    const dayGroups = {};
    phTrendData.forEach((data, index) => {
        const day = data.date.split(' ')[0];
        if (!dayGroups[day]) {
            dayGroups[day] = [];
        }
        dayGroups[day].push({...data, originalIndex: index});
    });
    
    // Sortiere Daten innerhalb jedes Tages nach Zeit
    Object.keys(dayGroups).forEach(day => {
        dayGroups[day].sort((a, b) => {
            const timeA = a.date.split(' ')[1] || '12:00';
            const timeB = b.date.split(' ')[1] || '12:00';
            return timeA.localeCompare(timeB);
        });
    });
    
    // KORREKTUR: Verwende die Backend-Markierung für recent
    const recent12 = phTrendData.filter(data => data.isRecent === true);
    
    return { dayGroups, recent12 };
},

drawPhTrendChart() {
    if (!this.phTrendData?.length) return;
    
    const svg = document.querySelector('#phChartContent');
    const chartWidth = 720;
    const chartHeight = 240;
    
    // Clear existing content
    svg.innerHTML = '';
    
    // Verarbeite Daten
    const { dayGroups, recent12 } = this.processPhDataForSvg(this.phTrendData);
    
    // Finde pH-Wertbereich
    const phValues = this.phTrendData.map(d => d.value);
    const minPH = Math.min(...phValues);
    const maxPH = Math.max(...phValues);
    
    // Padding für bessere Darstellung
    const phRange = maxPH - minPH;
    const paddedMin = Math.max(6.0, minPH - phRange * 0.1);
    const paddedMax = Math.min(9.0, maxPH + phRange * 0.1);
    
    // Skalierungsfunktionen
    const timeToHours = (timeStr) => {
        const [hours, minutes] = timeStr.split(':').map(Number);
        return hours + minutes / 60;
    };
    
    const xScale = (hours) => (hours / 24) * chartWidth;
    const yScale = (value) => chartHeight - ((value - paddedMin) / (paddedMax - paddedMin)) * chartHeight;
    
    // GEÄNDERT: Zeichne Linien für jeden Tag mit neuem Farbverlauf Orange->Blau
    const sortedDays = Object.keys(dayGroups).sort((a, b) => {
        const parseDate = (dateStr) => {
            const [day, month, year] = dateStr.split('.');
            return new Date(`20${year}`, month - 1, day);
        };
        return parseDate(b) - parseDate(a);  // Neueste zuerst (b - a)
    });

    // GEÄNDERT: Reihenfolge umkehren, damit älteste Linien zuerst (im Hintergrund) 
    // und neueste Linien zuletzt (im Vordergrund) gezeichnet werden
    sortedDays.reverse().forEach((day, dayIndex) => {
        const dayData = dayGroups[day];
        const totalDays = sortedDays.length;
        
        // GEÄNDERT: Altersberechnung angepasst für umgekehrte Reihenfolge
        // dayIndex 0 = ältester Tag (ageRatio = 0, Orange)
        // dayIndex (totalDays-1) = neuester Tag (ageRatio = 1, Blau)
        const ageRatio = dayIndex / Math.max(totalDays - 1, 1);
        const lineWidth = 1 + (ageRatio * 2);
        
        // GEÄNDERT: Farbverlauf getauscht - Blau nach Orange
        // Blau: rgb(0, 100, 255) für älteste Messungen (ageRatio = 0)
        // Orange: rgb(255, 165, 0) für neueste Messungen (ageRatio = 1)
        const red = Math.round(255 * ageRatio);          // 0 -> 255
        const green = Math.round(100 + 65 * ageRatio);   // 100 -> 165
        const blue = Math.round(255 - 255 * ageRatio);   // 255 -> 0
        const color = `rgb(${red}, ${green}, ${blue})`;  // Keine Transparenz
        
        let pathData = '';
        dayData.forEach((point, pointIndex) => {
            const timeStr = point.date.split(' ')[1] || '12:00';
            const hours = timeToHours(timeStr);
            const x = xScale(hours);
            const y = yScale(point.value);
            pathData += (pointIndex === 0 ? 'M ' : 'L ') + x + ' ' + y + ' ';
        });
        
        if (pathData) {
            const path = document.createElementNS('http://www.w3.org/2000/svg', 'path');
            path.setAttribute('d', pathData);
            path.setAttribute('stroke', color);
            path.setAttribute('stroke-width', lineWidth);
            path.setAttribute('fill', 'none');
            path.innerHTML = `<title>pH-Werte vom ${day}</title>`;
            svg.appendChild(path);
        }
    });
    
    // Zeichne hervorgehobene Linie für letzte 12 Messungen
    if (recent12.length > 0) {
        // Gruppiere recent12 auch nach Tagen
        const recentByDay = {};
        recent12.forEach(point => {
            const day = point.date.split(' ')[0];
            if (!recentByDay[day]) {
                recentByDay[day] = [];
            }
            recentByDay[day].push(point);
        });
        
        // GEÄNDERT: Verwende dunkles Orange für recent12 (passend zum neuen Farbschema)
        Object.keys(recentByDay).forEach(day => {
            const dayData = recentByDay[day];
            
            let recentPath = '';
            dayData.forEach((point, index) => {
                const timeStr = point.date.split(' ')[1] || '12:00';
                const hours = timeToHours(timeStr);
                const x = xScale(hours);
                const y = yScale(point.value);
                recentPath += (index === 0 ? 'M ' : 'L ') + x + ' ' + y + ' ';
            });
            
            if (recentPath) {
                const path = document.createElementNS('http://www.w3.org/2000/svg', 'path');
                path.setAttribute('d', recentPath);
                path.setAttribute('stroke', '#cc4400'); // GEÄNDERT: Dunkles Orange statt Dunkelblau
                path.setAttribute('stroke-width', '6');
                path.setAttribute('fill', 'none');
                path.innerHTML = `<title>Letzte Messungen vom ${day}</title>`;
                svg.appendChild(path);
            }
        });
    }

    // Aktualisierte Y-Achsen Labels
    const yAxis = document.querySelector('#phYAxis');
    yAxis.querySelectorAll('.ph-scale-label').forEach(el => el.remove());
    
    for (let i = 0; i <= 4; i++) {
        const value = paddedMin + (paddedMax - paddedMin) * (i / 4);
        const y = 20 + chartHeight - (i / 4) * chartHeight;
        const text = document.createElementNS('http://www.w3.org/2000/svg', 'text');
        text.setAttribute('x', '-35');
        text.setAttribute('y', y + 4);
        text.setAttribute('font-size', '10');
        text.setAttribute('fill', '#4CAF50');
        text.setAttribute('text-anchor', 'end');
        text.setAttribute('class', 'ph-scale-label');
        text.setAttribute('font-weight', 'bold');
        text.textContent = value.toFixed(1);
        yAxis.appendChild(text);
    }
    
    // X-Achsen Labels (Stunden)
    const xAxis = document.querySelector('#phXAxis');
    xAxis.querySelectorAll('.ph-time-label').forEach(el => el.remove());
    
    for (let hour = 0; hour <= 24; hour += 4) {
        const x = xScale(hour);
        const text = document.createElementNS('http://www.w3.org/2000/svg', 'text');
        text.setAttribute('x', x);
        text.setAttribute('y', '15');
        text.setAttribute('font-size', '10');
        text.setAttribute('fill', '#666');
        text.setAttribute('text-anchor', 'middle');
        text.setAttribute('class', 'ph-time-label');
        text.setAttribute('font-weight', 'bold');
        text.textContent = hour + ':00';
        xAxis.appendChild(text);
    }
},
},
mounted() {
    // Connect to WebSocket when the app is mounted
    this.connectWebSocket();

    // Set default values
    this.calibration.steps = 1000;
    this.globalSettings.speedML = 3.6;
    this.globalSettings.accelerationML = 1.8;

// Initialize with dashboard tab
    this.setActiveTab('dashboard');

    // Speicherstatus und WiFi-Config beim Start anfordern
    setTimeout(() => {
        this.requestMemoryStatus();
        this.loadWiFiConfig();
    }, 3000);

    // =========== VISIBILITY CHANGE HANDLER ===========
    // Wenn der Browser-Tab in den Hintergrund geht, kann der Browser
    // die WebSocket-Verbindung drosseln oder schließen.
    // Bei Rückkehr sofort Verbindung prüfen und ggf. reconnecten.
    document.addEventListener('visibilitychange', () => {
        if (document.visibilityState === 'visible') {
            console.log('[Visibility] Tab wieder sichtbar — prüfe WS-Verbindung');
            // Haupt-WS prüfen und ggf. sofort reconnecten
            if (!this.socket || this.socket.readyState !== WebSocket.OPEN) {
                console.log('[Visibility] Haupt-WS nicht verbunden → sofort reconnecten');
                this.wsReconnectDelay = 5000;  // Reset backoff
                this.connectWebSocket();
            } else {
                // Verbindung scheint noch offen — Test-Ping senden
                // Falls die Verbindung eigentlich tot ist, triggert das onclose
                try {
                    this.sendMessage({action: 'ping'});
                } catch (e) {
                    console.log('[Visibility] Ping fehlgeschlagen → reconnecte');
                    this.connectWebSocket();
                }
                // Tab-Daten aktualisieren (könnten veraltet sein)
                setTimeout(() => {
                    if (this.socket && this.socket.readyState === WebSocket.OPEN) {
                        this.setActiveTab(this.activeTab);
                    }
                }, 500);
            }
        }
    });

    // Client-seitiges Keepalive: alle 25s prüfen ob WS noch lebt
    setInterval(() => {
        if (document.visibilityState === 'visible' && this.socket) {
            if (this.socket.readyState === WebSocket.OPEN) {
                try {
                    this.sendMessage({action: 'ping'});
                } catch (e) {
                    console.log('[Keepalive] Ping fehlgeschlagen');
                }
            } else if (this.socket.readyState === WebSocket.CLOSED) {
                console.log('[Keepalive] WS geschlossen → reconnecte');
                this.wsReconnectDelay = 5000;
                this.connectWebSocket();
            }
        }
    }, 25000);
}
  }).mount('#app');
});
      </script>
   </body>
</html>
)=====";

#endif
