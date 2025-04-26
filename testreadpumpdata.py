import pandas as pd
import matplotlib.pyplot as plt

# 1) Read CSV
df = pd.read_csv('table_data_3_rows_2.csv', sep=None, engine='python')

# 2) Ensure doc_number is numeric
df['doc_number'] = pd.to_numeric(df['doc_number'], errors='coerce')

# 3) Convert current to amperes and compute ratio
df['current_A'] = df['current_mA'] / 1000
df['flow_current_ratio'] = df['current_A'] / df['flow_rate'] 

# 4) Plot
fig, ax1 = plt.subplots(figsize=(8,4))

# Left axis: current and flow rate
ax1.plot(df['doc_number'], df['current_A'],  marker='o', linestyle='-', label='Current (A)')
ax1.plot(df['doc_number'], df['flow_rate'], marker='s', linestyle='-', label='Flow Rate')
ax1.set_xlabel('Document Number')
ax1.set_ylabel('Current (A) / Flow Rate', fontsize=10)
ax1.legend(loc='upper left')

# Right axis: ratio
ax2 = ax1.twinx()
ax2.plot(df['doc_number'], df['flow_current_ratio'],
         marker='^', linestyle='--', color='red', label='Flow / Current', linewidth=1)
ax2.set_ylabel('Flow Rate / Current (unitless)', fontsize=10)
ax2.legend(loc='upper right')

plt.title('Pump Current, Flow Rate, and Their Ratio')
plt.tight_layout()
plt.show()